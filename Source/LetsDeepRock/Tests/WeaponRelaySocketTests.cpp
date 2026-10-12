// WeaponRelaySocket 对真实 TURN(pion)服务器的端到端自动化测试
//
// 需要环境变量(未设置时跳过):
//   LDR_RELAY_TEST_SERVER  TURN 服务器内网地址,如 192.168.3.27:3478
//   LDR_RELAY_TEST_SECRET  服务器 RELAY_SECRET(用于按 TURN REST 规则签发凭证)
//
// 两个 relay socket 各自 Allocate;B 经服务器内网 IP + A 的 relayed 端口发包,
// pion 在本机转发,不经过路由器 hairpin,只验证本端 TURN 协议实现:
// XOR 地址编码、permission、Send/Data Indication、ChannelBind/ChannelData、源地址回填。

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Online/WeaponRelaySocket.h"
#include "Online/WeaponCandidateProber.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Misc/Base64.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"

// 与 WeaponRelaySocket.cpp 相同:OpenSSL 的 UI typedef 与 UE 的 namespace UI 冲突。
#define UI OpenSSL_UI_Type_Unused
THIRD_PARTY_INCLUDES_START
#include <openssl/hmac.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace WeaponRelaySocketTests
{
	// TURN REST 凭证:username = "expiry:match:peer",password = base64(HMAC-SHA1(secret, username))
	void MakeCredential(const FString& Secret, const FString& PeerId, FString& OutUser, FString& OutPass)
	{
		const int64 Expiry = FDateTime::UtcNow().ToUnixTimestamp() + 600;
		OutUser = FString::Printf(TEXT("%lld:relaytest-match:%s"), Expiry, *PeerId);
		FTCHARToUTF8 Key(*Secret);
		FTCHARToUTF8 Msg(*OutUser);
		uint8 Digest[20];
		uint32 DigestLen = 0;
		HMAC(EVP_sha1(), Key.Get(), Key.Length(), reinterpret_cast<const uint8*>(Msg.Get()), Msg.Length(), Digest, &DigestLen);
		OutPass = FBase64::Encode(Digest, DigestLen);
	}

	FWeaponRelaySocket* MakeRelay(ISocketSubsystem* Sockets, const FString& Server, const FString& Secret, const FString& PeerId)
	{
		FString User, Pass;
		MakeCredential(Secret, PeerId, User, Pass);
		FSocket* Raw = Sockets->CreateSocket(NAME_DGram, TEXT("RelayTestUnderlying"), FNetworkProtocolTypes::IPv4);
		Raw->SetNonBlocking(true);
		FWeaponRelaySocket* Relay = new FWeaponRelaySocket(Raw, TEXT("WeaponRelay"));
		Relay->Allocate(Server, User, Pass);
		TSharedRef<FInternetAddr> Any = Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
		Any->SetAnyAddress();
		Relay->Bind(*Any);
		return Relay;
	}

	/** 抽干 socket;读到数据包时返回 true 并输出 payload 与源地址。 */
	bool Pump(FWeaponRelaySocket& Socket, FString& OutPayload, FInternetAddr& OutFrom)
	{
		uint8 Buffer[2048];
		int32 Read = 0;
		while (Socket.RecvFrom(Buffer, sizeof(Buffer), Read, OutFrom))
		{
			if (Read > 0)
			{
				OutPayload = FString(Read, reinterpret_cast<const char*>(Buffer));
				return true;
			}
		}
		return false;
	}

	bool SameEndpoint(const FInternetAddr& A, const FInternetAddr& B)
	{
		uint32 IpA = 0, IpB = 0;
		A.GetIp(IpA);
		B.GetIp(IpB);
		return IpA == IpB && A.GetPort() == B.GetPort();
	}

	/** 持续抽干两端(驱动 Tick:重传/刷新/保活)指定时长,或直到双方都 Ready。 */
	bool PumpBoth(FWeaponRelaySocket& A, FWeaponRelaySocket& B, double Seconds, bool bStopWhenReady)
	{
		FString Ignored;
		TSharedRef<FInternetAddr> Scratch = ISocketSubsystem::Get()->CreateInternetAddr();
		const double Deadline = FPlatformTime::Seconds() + Seconds;
		while (FPlatformTime::Seconds() < Deadline)
		{
			if (bStopWhenReady && A.GetState() == EWeaponRelaySocketState::Ready && B.GetState() == EWeaponRelaySocketState::Ready)
			{
				return true;
			}
			Pump(A, Ignored, *Scratch);
			Pump(B, Ignored, *Scratch);
			FPlatformProcess::Sleep(0.02f);
		}
		return A.GetState() == EWeaponRelaySocketState::Ready && B.GetState() == EWeaponRelaySocketState::Ready;
	}

	/** 反复发送直到对端收到同一 payload(permission/bind 安装期间允许丢首包)。 */
	bool SendUntilReceived(FWeaponRelaySocket& From, FWeaponRelaySocket& To, const FInternetAddr& Dest, const FString& Payload, FInternetAddr& OutSource)
	{
		FTCHARToUTF8 Utf8(*Payload);
		const double Deadline = FPlatformTime::Seconds() + 5.0;
		double NextSend = 0.0;
		while (FPlatformTime::Seconds() < Deadline)
		{
			if (FPlatformTime::Seconds() >= NextSend)
			{
				int32 Sent = 0;
				From.SendTo(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Sent, Dest);
				NextSend = FPlatformTime::Seconds() + 0.1;
			}
			// 发送方也要抽干,以处理它自己的 TURN 响应(ChannelBind 成功等)
			FString Ignored;
			TSharedRef<FInternetAddr> Scratch = ISocketSubsystem::Get()->CreateInternetAddr();
			Pump(From, Ignored, *Scratch);
			FString Received;
			if (Pump(To, Received, OutSource) && Received == Payload)
			{
				return true;
			}
			FPlatformProcess::Sleep(0.01f);
		}
		return false;
	}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeaponRelaySocketLiveTurnTest, "LetsDeepRock.Online.RelaySocket.LiveTurnRoundTrip",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWeaponRelaySocketLiveTurnTest::RunTest(const FString& Parameters)
{
	using namespace WeaponRelaySocketTests;

	const FString Server = FPlatformMisc::GetEnvironmentVariable(TEXT("LDR_RELAY_TEST_SERVER"));
	const FString Secret = FPlatformMisc::GetEnvironmentVariable(TEXT("LDR_RELAY_TEST_SECRET"));
	if (Server.IsEmpty() || Secret.IsEmpty())
	{
		AddInfo(TEXT("LDR_RELAY_TEST_SERVER / LDR_RELAY_TEST_SECRET not set; skipping live TURN test"));
		return true;
	}

	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TUniquePtr<FWeaponRelaySocket> A(MakeRelay(Sockets, Server, Secret, TEXT("peer-a")));
	TUniquePtr<FWeaponRelaySocket> B(MakeRelay(Sockets, Server, Secret, TEXT("peer-b")));

	// 1) 双方 Allocate 完成
	const double AllocDeadline = FPlatformTime::Seconds() + 8.0;
	FString Ignored;
	TSharedRef<FInternetAddr> Scratch = Sockets->CreateInternetAddr();
	while (FPlatformTime::Seconds() < AllocDeadline &&
		(A->GetState() != EWeaponRelaySocketState::Ready || B->GetState() != EWeaponRelaySocketState::Ready))
	{
		Pump(*A, Ignored, *Scratch);
		Pump(*B, Ignored, *Scratch);
		FPlatformProcess::Sleep(0.02f);
	}
	if (!TestTrue(TEXT("A allocated"), A->GetState() == EWeaponRelaySocketState::Ready) ||
		!TestTrue(TEXT("B allocated"), B->GetState() == EWeaponRelaySocketState::Ready))
	{
		return false;
	}
	AddInfo(FString::Printf(TEXT("A relayed %s, B relayed %s"),
		*A->GetRelayedAddress()->ToString(true), *B->GetRelayedAddress()->ToString(true)));

	// 服务器写 IP 时经该 IP + A 的 relay 端口访问(内网 IP 则 pion 本机转发,不走 hairpin);
	// 写域名时直接用 A 的公网 relayed 地址,与游戏实际路径一致。
	TSharedRef<FInternetAddr> ALan = A->GetRelayedAddress()->Clone();
	const FString ServerHost = Server.Contains(TEXT(":")) ? Server.Left(Server.Find(TEXT(":"))) : Server;
	bool bValidIp = false;
	TSharedRef<FInternetAddr> ServerIp = Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
	ServerIp->SetIp(*ServerHost, bValidIp);
	if (bValidIp)
	{
		ALan->SetRawIp(ServerIp->GetRawIp());
	}

	// 2) B -> A:Send Indication / ChannelData 经 TURN 到达 A,A 看到的源是 B 的 relay 端口
	TSharedRef<FInternetAddr> SourceAtA = Sockets->CreateInternetAddr();
	if (!TestTrue(TEXT("B->A first packet delivered"), SendUntilReceived(*B, *A, *ALan, TEXT("hello-from-b"), *SourceAtA)))
	{
		return false;
	}
	TestEqual(TEXT("A sees B's relay port"), SourceAtA->GetPort(), B->GetRelayedAddress()->GetPort());

	// 3) A -> B 回包:B 看到的源必须正好是它发往的地址,否则 NetConnection 无法匹配
	TSharedRef<FInternetAddr> SourceAtB = Sockets->CreateInternetAddr();
	if (!TestTrue(TEXT("A->B reply delivered"), SendUntilReceived(*A, *B, *SourceAtA, TEXT("hello-from-a"), *SourceAtB)))
	{
		return false;
	}
	TestTrue(TEXT("B sees reply from the address it sent to"), SameEndpoint(*SourceAtB, *ALan));

	// 4) 多轮往返:覆盖 ChannelBind 完成后的 ChannelData 路径
	for (int32 Round = 0; Round < 20; ++Round)
	{
		TSharedRef<FInternetAddr> SrcAtA = Sockets->CreateInternetAddr();
		TSharedRef<FInternetAddr> SrcAtB = Sockets->CreateInternetAddr();
		const FString Ping = FString::Printf(TEXT("ping-%d"), Round);
		const FString Pong = FString::Printf(TEXT("pong-%d"), Round);
		if (!TestTrue(*FString::Printf(TEXT("round %d ping"), Round), SendUntilReceived(*B, *A, *ALan, Ping, *SrcAtA)) ||
			!TestTrue(*FString::Printf(TEXT("round %d pong"), Round), SendUntilReceived(*A, *B, *SrcAtA, Pong, *SrcAtB)))
		{
			return false;
		}
	}
	return true;
}

// 回归:Host 分配后长时间无流量,NAT/代理 UDP 映射过期导致源地址变化,
// TURN 按旧源地址转发,对端首包永远到不了(2026-10-12 "All candidates failed")。
// 空闲时长默认 90s,可用 LDR_RELAY_TEST_IDLE_SECONDS 覆盖。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeaponRelaySocketIdleKeepaliveTest, "LetsDeepRock.Online.RelaySocket.LiveTurnIdleKeepalive",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWeaponRelaySocketIdleKeepaliveTest::RunTest(const FString& Parameters)
{
	using namespace WeaponRelaySocketTests;

	const FString Server = FPlatformMisc::GetEnvironmentVariable(TEXT("LDR_RELAY_TEST_SERVER"));
	const FString Secret = FPlatformMisc::GetEnvironmentVariable(TEXT("LDR_RELAY_TEST_SECRET"));
	if (Server.IsEmpty() || Secret.IsEmpty())
	{
		AddInfo(TEXT("LDR_RELAY_TEST_SERVER / LDR_RELAY_TEST_SECRET not set; skipping live TURN test"));
		return true;
	}
	const FString IdleEnv = FPlatformMisc::GetEnvironmentVariable(TEXT("LDR_RELAY_TEST_IDLE_SECONDS"));
	const double IdleSeconds = IdleEnv.IsEmpty() ? 90.0 : FCString::Atod(*IdleEnv);

	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TUniquePtr<FWeaponRelaySocket> Host(MakeRelay(Sockets, Server, Secret, TEXT("peer-host")));
	TUniquePtr<FWeaponRelaySocket> Client(MakeRelay(Sockets, Server, Secret, TEXT("peer-client")));
	if (!TestTrue(TEXT("both allocated"), PumpBoth(*Host, *Client, 8.0, true)))
	{
		return false;
	}

	// 与游戏一致:Host 发布 relayed 地址后空闲等待客户端
	AddInfo(FString::Printf(TEXT("Host relayed %s; idling %.0fs"), *Host->GetRelayedAddress()->ToString(true), IdleSeconds));
	PumpBoth(*Host, *Client, IdleSeconds, false);

	TSharedRef<FInternetAddr> HostRelayed = Host->GetRelayedAddress()->Clone();
	TSharedRef<FInternetAddr> SourceAtHost = Sockets->CreateInternetAddr();
	if (!TestTrue(TEXT("client reaches idle host"), SendUntilReceived(*Client, *Host, *HostRelayed, TEXT("join"), *SourceAtHost)))
	{
		return false;
	}
	TSharedRef<FInternetAddr> SourceAtClient = Sockets->CreateInternetAddr();
	TestTrue(TEXT("host replies"), SendUntilReceived(*Host, *Client, *SourceAtHost, TEXT("welcome"), *SourceAtClient));
	TestTrue(TEXT("client sees host relayed address"), SameEndpoint(*SourceAtClient, *HostRelayed));
	return true;
}

// 并行测速:Host relay socket 对 direct(127.0.0.1 + 底层端口)与 relay(relayed 地址)两条路径都回 pong,
// 测速器拿到两者 RTT,且本机直连快于经 TURN 中转。
IMPLEMENT_SIMPLE_AUTOMATION_TEST(FWeaponCandidateProberLiveTest, "LetsDeepRock.Online.RelaySocket.LiveCandidatePing",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FWeaponCandidateProberLiveTest::RunTest(const FString& Parameters)
{
	using namespace WeaponRelaySocketTests;

	const FString Server = FPlatformMisc::GetEnvironmentVariable(TEXT("LDR_RELAY_TEST_SERVER"));
	const FString Secret = FPlatformMisc::GetEnvironmentVariable(TEXT("LDR_RELAY_TEST_SECRET"));
	if (Server.IsEmpty() || Secret.IsEmpty())
	{
		AddInfo(TEXT("LDR_RELAY_TEST_SERVER / LDR_RELAY_TEST_SECRET not set; skipping live TURN test"));
		return true;
	}

	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TUniquePtr<FWeaponRelaySocket> Host(MakeRelay(Sockets, Server, Secret, TEXT("peer-host")));
	FString Ignored;
	TSharedRef<FInternetAddr> Scratch = Sockets->CreateInternetAddr();
	const double AllocDeadline = FPlatformTime::Seconds() + 8.0;
	while (Host->GetState() != EWeaponRelaySocketState::Ready && FPlatformTime::Seconds() < AllocDeadline)
	{
		Pump(*Host, Ignored, *Scratch);
		FPlatformProcess::Sleep(0.02f);
	}
	if (!TestTrue(TEXT("host allocated"), Host->GetState() == EWeaponRelaySocketState::Ready))
	{
		return false;
	}

	FString User, Pass;
	MakeCredential(Secret, TEXT("peer-client"), User, Pass);
	const FInternetAddr* Relayed = Host->GetRelayedAddress();
	FWeaponCandidateProber Prober;
	Prober.AddDirectTarget(0, TEXT("127.0.0.1"), Host->GetPortNo());
	Prober.AddRelayTarget(1, Relayed->ToString(false), Relayed->GetPort(), Server, User, Pass);
	if (!TestTrue(TEXT("prober started"), Prober.Start(5.0)))
	{
		return false;
	}
	const double Deadline = FPlatformTime::Seconds() + 6.0;
	bool bDone = false;
	while (!bDone && FPlatformTime::Seconds() < Deadline)
	{
		bDone = Prober.Tick();
		Pump(*Host, Ignored, *Scratch); // Host 侧回 pong;ping 不会作为数据包交出
		FPlatformProcess::Sleep(0.005f);
	}

	const TArray<FWeaponCandidateProber::FTarget>& Targets = Prober.GetTargets();
	AddInfo(FString::Printf(TEXT("direct %.1f ms, relay %.1f ms"), Targets[0].RttSeconds * 1000.0, Targets[1].RttSeconds * 1000.0));
	TestTrue(TEXT("direct answered"), Targets[0].RttSeconds >= 0.0);
	TestTrue(TEXT("relay answered"), Targets[1].RttSeconds >= 0.0);
	TestTrue(TEXT("loopback direct faster than relay"), Targets[0].RttSeconds < Targets[1].RttSeconds);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
