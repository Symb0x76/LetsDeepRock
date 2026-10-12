// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponRelaySocket.h"
#include "WeaponConnectProbe.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "Misc/ScopeLock.h"

// OpenSSL 的 `typedef struct ui_st UI` 与 UE 的 `namespace UI` 冲突。
// 只在 OpenSSL include 前把 UI 改名为无冲突标识符;本文件不调用任何 UI API。
#define UI OpenSSL_UI_Type_Unused
THIRD_PARTY_INCLUDES_START
#include <openssl/hmac.h>
#include <openssl/md5.h>
THIRD_PARTY_INCLUDES_END
#undef UI

DECLARE_LOG_CATEGORY_EXTERN(LogWeaponRelay, Log, All);
DEFINE_LOG_CATEGORY(LogWeaponRelay);

// ---- STUN constants (RFC 5389 / 5766) ----

namespace WeaponRelayStun
{
	constexpr uint32 MagicCookie32 = 0x2112A442;

	// methods(类别位已剥离)
	constexpr uint16 MethodBinding          = 0x0001;
	constexpr uint16 MethodAllocate         = 0x0003;
	constexpr uint16 MethodRefresh          = 0x0004;
	constexpr uint16 MethodSend             = 0x0006;
	constexpr uint16 MethodData             = 0x0007;
	constexpr uint16 MethodCreatePermission = 0x0008;
	constexpr uint16 MethodChannelBind      = 0x0009;

	// message classes
	constexpr uint16 ClassMask       = 0x0110;
	constexpr uint16 ClassRequest    = 0x0000;
	constexpr uint16 ClassIndication = 0x0010;
	constexpr uint16 ClassSuccess    = 0x0100;
	constexpr uint16 ClassError      = 0x0110;

	// attribute types
	constexpr uint16 AttrUsername            = 0x0006;
	constexpr uint16 AttrMessageIntegrity    = 0x0008;
	constexpr uint16 AttrErrorCode           = 0x0009;
	constexpr uint16 AttrChannelNumber       = 0x000C;
	constexpr uint16 AttrLifetime            = 0x000D;
	constexpr uint16 AttrXorPeerAddress      = 0x0012;
	constexpr uint16 AttrData                = 0x0013;
	constexpr uint16 AttrRealm               = 0x0014;
	constexpr uint16 AttrNonce               = 0x0015;
	constexpr uint16 AttrXorRelayedAddress   = 0x0016;
	constexpr uint16 AttrRequestedTransport  = 0x0019;

	constexpr int32 HeaderSize = 20;

	// ChannelData:首 2 bit 为 01,channel 号 0x4000-0x7FFE
	constexpr uint16 ChannelDataMask    = 0xC000;
	constexpr uint16 ChannelDataMarker  = 0x4000;
	constexpr uint16 ChannelMin         = 0x4000;
	constexpr uint16 ChannelMax         = 0x7FFE;

	// 生命周期(秒):Allocation 申请 600s 每 300s 刷新;permission 300s、channel
	// 600s 过期,每 120s 用 ChannelBind/CreatePermission 刷新(ChannelBind 同时刷新 permission)。
	constexpr uint32 RequestedLifetime        = 600;
	constexpr double AllocationRefreshSeconds = 300.0;
	constexpr double PermissionRefreshSeconds = 120.0;
	constexpr double KeepaliveSeconds         = 5.0;

	// 重传:RTO 0.5s 起倍增封顶 3s,总时长 10s 后放弃
	constexpr double InitialRto        = 0.5;
	constexpr double MaxRto            = 3.0;
	constexpr double TransactionBudget = 10.0;
}

namespace
{
	uint16 RelayReadU16(const uint8* P) { return static_cast<uint16>((P[0] << 8) | P[1]); }
	uint32 RelayReadU32(const uint8* P)
	{
		return (static_cast<uint32>(P[0]) << 24) | (static_cast<uint32>(P[1]) << 16) | (static_cast<uint32>(P[2]) << 8) | P[3];
	}
	void RelayWriteU16(TArray<uint8>& Out, uint16 V) { Out.Add(static_cast<uint8>(V >> 8)); Out.Add(static_cast<uint8>(V & 0xFF)); }
	void RelayWriteU32(TArray<uint8>& Out, uint32 V)
	{
		Out.Add(static_cast<uint8>(V >> 24)); Out.Add(static_cast<uint8>((V >> 16) & 0xFF));
		Out.Add(static_cast<uint8>((V >> 8) & 0xFF)); Out.Add(static_cast<uint8>(V & 0xFF));
	}

	/** 写属性并按 RFC 5389 §15 补齐到 4 字节边界。 */
	void RelayWriteAttr(TArray<uint8>& Out, uint16 Type, const uint8* Value, int32 Len)
	{
		RelayWriteU16(Out, Type);
		RelayWriteU16(Out, static_cast<uint16>(Len));
		Out.Append(Value, Len);
		while (Out.Num() % 4 != 0)
		{
			Out.Add(0);
		}
	}

	void RelayWriteStringAttr(TArray<uint8>& Out, uint16 Type, const FString& Value)
	{
		FTCHARToUTF8 Utf8(*Value);
		RelayWriteAttr(Out, Type, reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length());
	}

	void RelayPatchLength(TArray<uint8>& Msg)
	{
		const uint16 Len = static_cast<uint16>(Msg.Num() - WeaponRelayStun::HeaderSize);
		Msg[2] = static_cast<uint8>(Len >> 8);
		Msg[3] = static_cast<uint8>(Len & 0xFF);
	}

	/** 写 STUN 头(长度稍后回填),返回 transaction id 的 hex key。 */
	FString RelayWriteHeader(TArray<uint8>& Msg, uint16 MessageType)
	{
		RelayWriteU16(Msg, MessageType);
		RelayWriteU16(Msg, 0);
		RelayWriteU32(Msg, WeaponRelayStun::MagicCookie32);
		uint8 Tid[12];
		for (int32 i = 0; i < 12; ++i)
		{
			Tid[i] = static_cast<uint8>(FMath::RandRange(0, 255));
		}
		Msg.Append(Tid, 12);
		return BytesToHex(Tid, 12);
	}

	// XOR-*-ADDRESS(RFC 5389 §15.2,IPv4):X-Port = port ^ cookie 高 16 位;
	// X-Address = 地址(网络字节序) ^ cookie。FInternetAddr::GetIp 返回主机序
	// (首个八位组在最高字节),所以按 Ip >> 24 .. Ip 的顺序写出网络序字节。
	void RelayWriteXorAddr(TArray<uint8>& Out, uint16 AttrType, const FInternetAddr& Addr)
	{
		uint32 Ip = 0;
		Addr.GetIp(Ip);
		const uint16 XPort = static_cast<uint16>(Addr.GetPort()) ^ static_cast<uint16>(WeaponRelayStun::MagicCookie32 >> 16);
		const uint32 XIp = Ip ^ WeaponRelayStun::MagicCookie32;
		uint8 Value[8];
		Value[0] = 0;
		Value[1] = 0x01; // family: IPv4
		Value[2] = static_cast<uint8>(XPort >> 8);
		Value[3] = static_cast<uint8>(XPort & 0xFF);
		Value[4] = static_cast<uint8>(XIp >> 24);
		Value[5] = static_cast<uint8>((XIp >> 16) & 0xFF);
		Value[6] = static_cast<uint8>((XIp >> 8) & 0xFF);
		Value[7] = static_cast<uint8>(XIp & 0xFF);
		RelayWriteAttr(Out, AttrType, Value, 8);
	}

	//   [0]=reserved [1]=family [2..3]=X-Port [4..7]=X-Address
	void RelayDecodeXorAddr(const uint8* AttrData, FInternetAddr& OutAddr)
	{
		const uint16 Port = RelayReadU16(AttrData + 2) ^ static_cast<uint16>(WeaponRelayStun::MagicCookie32 >> 16);
		const uint32 Ip = RelayReadU32(AttrData + 4) ^ WeaponRelayStun::MagicCookie32;
		OutAddr.SetIp(Ip);
		OutAddr.SetPort(Port);
	}

	/** FInternetAddr 没有多态赋值:基类引用上 `A = B` 什么都不拷贝,必须逐字段设置。 */
	void RelayCopyAddr(FInternetAddr& Dst, const FInternetAddr& Src)
	{
		Dst.SetRawIp(Src.GetRawIp());
		Dst.SetPort(Src.GetPort());
	}

	// 键与比较统一走 GetIp(uint32):双栈 socket 读出的是 v4-mapped IPv6 地址,
	// 与 GetAddressInfo 解析出的 IPv4 地址 family 不同,operator== 会判为不等。
	FString RelayIpKey(const FInternetAddr& Addr)
	{
		uint32 Ip = 0;
		Addr.GetIp(Ip);
		return FString::Printf(TEXT("%u.%u.%u.%u"), (Ip >> 24) & 0xFF, (Ip >> 16) & 0xFF, (Ip >> 8) & 0xFF, Ip & 0xFF);
	}
	FString RelayPeerKey(const FInternetAddr& Addr)
	{
		return FString::Printf(TEXT("%s:%d"), *RelayIpKey(Addr), Addr.GetPort());
	}

	// RFC 5389 §15.4: key = MD5(username ":" realm ":" password)
	TArray<uint8> RelayMakeAuthKey(const FString& Username, const FString& Realm, const FString& Password)
	{
		const FString Combined = Username + TEXT(":") + Realm + TEXT(":") + Password;
		FTCHARToUTF8 Utf8(*Combined);
		TArray<uint8> Key;
		Key.SetNumUninitialized(16);
		MD5(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Key.GetData());
		return Key;
	}

	// 追加 MESSAGE-INTEGRITY(HMAC-SHA1,20 字节)。头部长度先改为含 MI 的长度,
	// HMAC 覆盖到 MI 属性头之前(RFC 5389 §15.4,pion 的 Check 按此实现)。
	void RelayAppendMessageIntegrity(TArray<uint8>& Msg, const TArray<uint8>& Key)
	{
		constexpr int32 HmacSize = 20;
		constexpr int32 AttrHeaderSize = 4;
		const int32 HmacInputLen = Msg.Num();
		const uint16 NewLen = static_cast<uint16>(HmacInputLen - WeaponRelayStun::HeaderSize + AttrHeaderSize + HmacSize);
		Msg[2] = static_cast<uint8>(NewLen >> 8);
		Msg[3] = static_cast<uint8>(NewLen & 0xFF);

		RelayWriteU16(Msg, WeaponRelayStun::AttrMessageIntegrity);
		RelayWriteU16(Msg, HmacSize);
		const int32 HmacOffset = Msg.Num();
		Msg.AddZeroed(HmacSize);

		uint32 HmacLen = 0;
		HMAC(EVP_sha1(), Key.GetData(), Key.Num(), Msg.GetData(), HmacInputLen, Msg.GetData() + HmacOffset, &HmacLen);
		check(HmacLen == HmacSize);
	}

	const TCHAR* RelayRequestName(uint16 Method)
	{
		switch (Method)
		{
		case WeaponRelayStun::MethodAllocate: return TEXT("Allocate");
		case WeaponRelayStun::MethodRefresh: return TEXT("Refresh");
		case WeaponRelayStun::MethodCreatePermission: return TEXT("CreatePermission");
		case WeaponRelayStun::MethodChannelBind: return TEXT("ChannelBind");
		default: return TEXT("?");
		}
	}
}

FWeaponRelaySocket::FWeaponRelaySocket(FSocket* InUnderlyingSocket, const FString& InDescription)
	: FSocket(SOCKTYPE_Datagram, InDescription, FNetworkProtocolTypes::IPv4)
	, Underlying(InUnderlyingSocket)
{
}

FWeaponRelaySocket::~FWeaponRelaySocket()
{
	Close();
}

// ---- TURN 控制面 ----

bool FWeaponRelaySocket::Allocate(const FString& InTurnServerAddr, const FString& InUsername, const FString& InPassword)
{
	FScopeLock Lock(&StateLock);
	if (!Underlying)
	{
		return false;
	}

	TSharedPtr<FInternetAddr> Addr;
	if (!ResolveTurnServerAddress(InTurnServerAddr, Addr))
	{
		UE_LOG(LogWeaponRelay, Error, TEXT("Allocate: cannot resolve TURN server %s"), *InTurnServerAddr);
		return false;
	}

	TurnServerAddr = Addr;
	Username = InUsername;
	Password = InPassword;
	State = EWeaponRelaySocketState::Allocating;

	// 延迟事务:若底层 socket 尚未绑定(UE 的 CreateSocketForProtocol 在
	// InitBase 的 InitBindSockets 之前调用本函数),立即 SendTo 会让 OS
	// 隐含绑定随机端口,后续 Bind(0.0.0.0:0) 冲突。已绑定则由 Bind 触发。
	if (bUnderlyingBound)
	{
		BeginAllocateTransaction();
	}
	return true;
}

void FWeaponRelaySocket::BeginAllocateTransaction()
{
	if (State != EWeaponRelaySocketState::Allocating || !Underlying)
	{
		return;
	}
	for (const TPair<FString, FPendingRequest>& Pair : PendingRequests)
	{
		if (Pair.Value.Kind == ERequestKind::Allocate)
		{
			return;
		}
	}
	// 首个 Allocate 不带认证;服务器回 401 + NONCE/REALM 后带认证重发。
	SendRequest(ERequestKind::Allocate);
}

bool FWeaponRelaySocket::ResolveTurnServerAddress(const FString& InTurnServerAddr, TSharedPtr<FInternetAddr>& OutAddr) const
{
	OutAddr.Reset();
	ISocketSubsystem* Sockets = ISocketSubsystem::Get();
	if (!Sockets)
	{
		return false;
	}

	FString Host = InTurnServerAddr.TrimStartAndEnd();
	int32 Port = 3478;

	// 拆出 [IPv6]:port / host:port / 裸 host。与 Go 端 normalizeRelayAddr
	// 对齐:RELAY_ADDR 统一为 host[:port],端口缺省 3478。
	if (Host.StartsWith(TEXT("[")))
	{
		const int32 CloseBracket = Host.Find(TEXT("]"));
		if (CloseBracket == INDEX_NONE)
		{
			return false;
		}
		const FString Remainder = Host.RightChop(CloseBracket + 1);
		Host = Host.Mid(1, CloseBracket - 1);
		if (Remainder.StartsWith(TEXT(":")))
		{
			Port = FCString::Atoi(*Remainder.RightChop(1));
		}
	}
	else
	{
		const int32 Colon = Host.Find(TEXT(":"));
		if (Colon != INDEX_NONE)
		{
			Port = FCString::Atoi(*Host.RightChop(Colon + 1));
			Host = Host.Left(Colon);
		}
	}
	if (Host.IsEmpty() || Port <= 0 || Port > 65535)
	{
		return false;
	}

	// GetAddressInfo 支持 IP 与域名,同时解析端口,避免手写 inet_pton。
	const FString Service = FString::Printf(TEXT("%d"), Port);
	FAddressInfoResult Result = Sockets->GetAddressInfo(*Host, *Service, EAddressInfoFlags::Default, NAME_None);
	if (Result.ReturnCode != SE_NO_ERROR || Result.Results.Num() == 0)
	{
		UE_LOG(LogWeaponRelay, Warning, TEXT("ResolveTurnServerAddress: cannot resolve %s:%d"), *Host, Port);
		return false;
	}

	OutAddr = Result.Results[0].Address->Clone();
	OutAddr->SetPort(Port);
	return OutAddr.IsValid();
}

bool FWeaponRelaySocket::BuildRequest(ERequestKind Kind, const FInternetAddr* Peer, uint16 Channel, TArray<uint8>& OutMsg, FString& OutTransactionKey) const
{
	uint16 Method = 0;
	switch (Kind)
	{
	case ERequestKind::Allocate:         Method = WeaponRelayStun::MethodAllocate; break;
	case ERequestKind::Refresh:          Method = WeaponRelayStun::MethodRefresh; break;
	case ERequestKind::Deallocate:       Method = WeaponRelayStun::MethodRefresh; break;
	case ERequestKind::CreatePermission: Method = WeaponRelayStun::MethodCreatePermission; break;
	case ERequestKind::ChannelBind:      Method = WeaponRelayStun::MethodChannelBind; break;
	}

	OutMsg.Reset();
	OutTransactionKey = RelayWriteHeader(OutMsg, Method | WeaponRelayStun::ClassRequest);

	switch (Kind)
	{
	case ERequestKind::Allocate:
	{
		const uint8 Transport[4] = { 17, 0, 0, 0 }; // REQUESTED-TRANSPORT: UDP
		RelayWriteAttr(OutMsg, WeaponRelayStun::AttrRequestedTransport, Transport, 4);
		break;
	}
	case ERequestKind::Refresh:
	case ERequestKind::Deallocate:
	{
		TArray<uint8> Lifetime;
		RelayWriteU32(Lifetime, Kind == ERequestKind::Deallocate ? 0u : WeaponRelayStun::RequestedLifetime);
		RelayWriteAttr(OutMsg, WeaponRelayStun::AttrLifetime, Lifetime.GetData(), 4);
		break;
	}
	case ERequestKind::CreatePermission:
		if (!Peer)
		{
			return false;
		}
		RelayWriteXorAddr(OutMsg, WeaponRelayStun::AttrXorPeerAddress, *Peer);
		break;
	case ERequestKind::ChannelBind:
	{
		if (!Peer)
		{
			return false;
		}
		TArray<uint8> ChannelValue;
		RelayWriteU16(ChannelValue, Channel);
		RelayWriteU16(ChannelValue, 0);
		RelayWriteAttr(OutMsg, WeaponRelayStun::AttrChannelNumber, ChannelValue.GetData(), 4);
		RelayWriteXorAddr(OutMsg, WeaponRelayStun::AttrXorPeerAddress, *Peer);
		break;
	}
	}

	// 已拿到 NONCE(首个 Allocate 的 401 之后)的请求全部带长期凭证认证。
	if (!Nonce.IsEmpty())
	{
		RelayWriteStringAttr(OutMsg, WeaponRelayStun::AttrUsername, Username);
		RelayWriteStringAttr(OutMsg, WeaponRelayStun::AttrRealm, Realm);
		RelayWriteStringAttr(OutMsg, WeaponRelayStun::AttrNonce, Nonce);
		RelayAppendMessageIntegrity(OutMsg, RelayMakeAuthKey(Username, Realm, Password));
	}
	else
	{
		RelayPatchLength(OutMsg);
	}
	return true;
}

void FWeaponRelaySocket::SendRequest(ERequestKind Kind, const TSharedPtr<FInternetAddr>& Peer, uint16 Channel)
{
	if (!Underlying || !TurnServerAddr.IsValid())
	{
		return;
	}

	FPendingRequest Request;
	Request.Kind = Kind;
	Request.Peer = Peer;
	Request.Channel = Channel;
	Request.bAuthenticated = !Nonce.IsEmpty();
	FString Key;
	if (!BuildRequest(Kind, Peer.Get(), Channel, Request.Message, Key))
	{
		return;
	}

	const double Now = FPlatformTime::Seconds();
	Request.FirstSentTime = Now;
	Request.LastSentTime = Now;
	Request.Attempts = 1;

	SendToTurn(Request.Message.GetData(), Request.Message.Num());
	UE_LOG(LogWeaponRelay, Verbose, TEXT("TURN %d request sent (peer=%s channel=0x%04x auth=%d)"),
		static_cast<int32>(Kind), Peer.IsValid() ? *Peer->ToString(true) : TEXT("-"), Channel, Request.bAuthenticated ? 1 : 0);
	PendingRequests.Add(Key, MoveTemp(Request));
}

void FWeaponRelaySocket::Tick()
{
	FScopeLock Lock(&StateLock);
	if (!Underlying || bFallbackDirect || State == EWeaponRelaySocketState::Closed || State == EWeaponRelaySocketState::Failed)
	{
		return;
	}
	const double Now = FPlatformTime::Seconds();

	// 重传 / 超时
	TArray<FString> Expired;
	for (TPair<FString, FPendingRequest>& Pair : PendingRequests)
	{
		FPendingRequest& Request = Pair.Value;
		if (Now - Request.FirstSentTime > WeaponRelayStun::TransactionBudget)
		{
			Expired.Add(Pair.Key);
			continue;
		}
		const double Rto = FMath::Min(WeaponRelayStun::InitialRto * FMath::Pow(2.0, static_cast<double>(Request.Attempts - 1)), WeaponRelayStun::MaxRto);
		if (Now - Request.LastSentTime >= Rto && TurnServerAddr.IsValid())
		{
			SendToTurn(Request.Message.GetData(), Request.Message.Num());
			Request.LastSentTime = Now;
			++Request.Attempts;
		}
	}
	for (const FString& Key : Expired)
	{
		FPendingRequest Request;
		PendingRequests.RemoveAndCopyValue(Key, Request);
		UE_LOG(LogWeaponRelay, Warning, TEXT("TURN %d request timed out after %d attempts (peer=%s)"),
			static_cast<int32>(Request.Kind), Request.Attempts, Request.Peer.IsValid() ? *Request.Peer->ToString(true) : TEXT("-"));
		// Allocation 本身失败才是致命的;permission/channel 下一轮刷新再试。
		if (Request.Kind == ERequestKind::Allocate || Request.Kind == ERequestKind::Refresh)
		{
			EnterFailedState();
			return;
		}
	}

	if (State != EWeaponRelaySocketState::Ready)
	{
		return;
	}

	// Allocation 刷新
	if (Now - LastAllocationRefreshTime >= WeaponRelayStun::AllocationRefreshSeconds)
	{
		LastAllocationRefreshTime = Now;
		SendRequest(ERequestKind::Refresh);
	}

	// permission 刷新(含 Allocate 完成前排队的预授权)
	for (TPair<FString, double>& Pair : Permissions)
	{
		if (Now - Pair.Value >= WeaponRelayStun::PermissionRefreshSeconds)
		{
			Pair.Value = Now;
			SendRequest(ERequestKind::CreatePermission, PermissionAddrs.FindRef(Pair.Key));
		}
	}

	// channel 刷新 / 未绑定重试(ChannelBind 同时刷新该 IP 的 permission)
	for (TPair<FString, FRelayPeer>& Pair : RelayPeers)
	{
		FRelayPeer& Peer = Pair.Value;
		if (Now - Peer.LastBindTime >= WeaponRelayStun::PermissionRefreshSeconds)
		{
			Peer.LastBindTime = Now;
			SendRequest(ERequestKind::ChannelBind, Peer.Addr, Peer.Channel);
		}
	}

	// NAT/代理保活:TURN 把 allocation 绑定在客户端源 IP:端口上。空闲 UDP 映射
	// 过期后,下一个包会以新的源地址出去,服务器上的 allocation 就再也收不到也发
	// 不到我们;即使外部端口不变,运营商 CGNAT/代理对"无出站后的入站"窗口也很短
	// (2026-10-12 实测 15s 保活时,Host 空闲后首个 Data Indication 被丢,5s 正常)。
	// 空闲时发 Binding 请求维持映射。
	if (Now - LastTurnSendTime >= WeaponRelayStun::KeepaliveSeconds)
	{
		TArray<uint8> Binding;
		RelayWriteHeader(Binding, WeaponRelayStun::MethodBinding | WeaponRelayStun::ClassRequest);
		RelayPatchLength(Binding);
		SendToTurn(Binding.GetData(), Binding.Num());
	}
}

bool FWeaponRelaySocket::SendToTurn(const uint8* Data, int32 Count)
{
	int32 Sent = 0;
	const bool bOk = Underlying->SendTo(Data, Count, Sent, *TurnServerAddr);
	LastTurnSendTime = FPlatformTime::Seconds();
	return bOk;
}

void FWeaponRelaySocket::EnterFailedState()
{
	State = EWeaponRelaySocketState::Failed;
	PendingRequests.Reset();
	if (!bEverReady)
	{
		bFallbackDirect = true;
		UE_LOG(LogWeaponRelay, Warning, TEXT("TURN allocation failed before ready; falling back to direct UDP"));
	}
	else
	{
		UE_LOG(LogWeaponRelay, Error, TEXT("TURN allocation lost; relayed traffic stops"));
	}
}

// ---- peer / permission ----

void FWeaponRelaySocket::EnsurePermission(const FInternetAddr& PeerAddr)
{
	const FString IpKey = RelayIpKey(PeerAddr);
	if (Permissions.Contains(IpKey))
	{
		return;
	}
	TSharedPtr<FInternetAddr> Addr = PeerAddr.Clone();
	PermissionAddrs.Add(IpKey, Addr);
	// 时间戳 0:Ready 后下一次 Tick 立即发送;已 Ready 则此刻发送。
	if (State == EWeaponRelaySocketState::Ready)
	{
		Permissions.Add(IpKey, FPlatformTime::Seconds());
		SendRequest(ERequestKind::CreatePermission, Addr);
	}
	else
	{
		Permissions.Add(IpKey, 0.0);
	}
	UE_LOG(LogWeaponRelay, Log, TEXT("TURN permission requested for %s"), *IpKey);
}

FWeaponRelaySocket::FRelayPeer& FWeaponRelaySocket::EnsurePeer(const FInternetAddr& PeerAddr)
{
	const FString Key = RelayPeerKey(PeerAddr);
	if (FRelayPeer* Existing = RelayPeers.Find(Key))
	{
		return *Existing;
	}

	FRelayPeer Peer;
	Peer.Addr = PeerAddr.Clone();
	Peer.Channel = NextChannel;
	NextChannel = (NextChannel >= WeaponRelayStun::ChannelMax) ? WeaponRelayStun::ChannelMin : NextChannel + 1;
	Peer.LastBindTime = FPlatformTime::Seconds();
	ChannelToPeer.Add(Peer.Channel, Key);

	// 先装 permission 让 Send Indication 立即可用,再并发 ChannelBind 切到 ChannelData。
	EnsurePermission(PeerAddr);
	SendRequest(ERequestKind::ChannelBind, Peer.Addr, Peer.Channel);
	UE_LOG(LogWeaponRelay, Log, TEXT("TURN peer %s -> channel 0x%04x"), *Key, Peer.Channel);
	return RelayPeers.Add(Key, MoveTemp(Peer));
}

bool FWeaponRelaySocket::IsFromTurnServer(const FInternetAddr& From) const
{
	return TurnServerAddr.IsValid() && RelayPeerKey(*TurnServerAddr) == RelayPeerKey(From);
}

// ---- 入站 STUN ----

void FWeaponRelaySocket::HandleStunMessage(const uint8* Data, int32 Size)
{
	const uint16 MsgType = RelayReadU16(Data);
	const uint16 MsgClass = MsgType & WeaponRelayStun::ClassMask;
	const uint16 Method = MsgType & ~WeaponRelayStun::ClassMask;
	if (MsgClass != WeaponRelayStun::ClassSuccess && MsgClass != WeaponRelayStun::ClassError)
	{
		return;
	}

	const FString Key = BytesToHex(Data + 8, 12);
	FPendingRequest Request;
	if (!PendingRequests.RemoveAndCopyValue(Key, Request))
	{
		return; // 重传导致的重复响应,或已超时放弃的事务
	}

	if (MsgClass == WeaponRelayStun::ClassSuccess)
	{
		switch (Request.Kind)
		{
		case ERequestKind::Allocate:
			HandleAllocateSuccess(Data, Size);
			break;
		case ERequestKind::Refresh:
		case ERequestKind::Deallocate:
			UE_LOG(LogWeaponRelay, Verbose, TEXT("TURN allocation refreshed"));
			break;
		case ERequestKind::CreatePermission:
			UE_LOG(LogWeaponRelay, Log, TEXT("TURN permission installed for %s"),
				Request.Peer.IsValid() ? *Request.Peer->ToString(false) : TEXT("?"));
			break;
		case ERequestKind::ChannelBind:
			if (Request.Peer.IsValid())
			{
				if (FRelayPeer* Peer = RelayPeers.Find(RelayPeerKey(*Request.Peer)))
				{
					if (!Peer->bChannelBound)
					{
						UE_LOG(LogWeaponRelay, Log, TEXT("TURN channel 0x%04x bound to %s"), Peer->Channel, *Request.Peer->ToString(true));
					}
					Peer->bChannelBound = true;
				}
			}
			break;
		}
		return;
	}

	// 错误响应:解析 ERROR-CODE / NONCE / REALM
	int32 ErrorCode = 0;
	FString ErrorReason;
	FString NewNonce;
	FString NewRealm;
	int32 Offset = WeaponRelayStun::HeaderSize;
	while (Offset + 4 <= Size)
	{
		const uint16 AttrType = RelayReadU16(Data + Offset);
		const uint16 AttrLen = RelayReadU16(Data + Offset + 2);
		if (Offset + 4 + AttrLen > Size)
		{
			break;
		}
		const uint8* AttrData = Data + Offset + 4;
		// STUN 属性值是 UTF-8 字节序列,必须按 char 构造 FString。
		if (AttrType == WeaponRelayStun::AttrErrorCode && AttrLen >= 4)
		{
			ErrorCode = (AttrData[2] & 0x07) * 100 + AttrData[3];
			ErrorReason = FString(AttrLen - 4, reinterpret_cast<const char*>(AttrData + 4));
		}
		else if (AttrType == WeaponRelayStun::AttrNonce)
		{
			NewNonce = FString(AttrLen, reinterpret_cast<const char*>(AttrData));
		}
		else if (AttrType == WeaponRelayStun::AttrRealm)
		{
			NewRealm = FString(AttrLen, reinterpret_cast<const char*>(AttrData));
		}
		Offset += 4 + ((AttrLen + 3) & ~3);
	}

	// 401(首个未认证 Allocate)或 438(nonce 过期):更新 nonce 后同参数重建请求。
	// 已带认证仍 401 说明凭证本身错误,不再重试。
	const bool bRetryWithNonce = !NewNonce.IsEmpty() &&
		((ErrorCode == 401 && !Request.bAuthenticated) || ErrorCode == 438);
	if (bRetryWithNonce)
	{
		Nonce = NewNonce;
		if (!NewRealm.IsEmpty())
		{
			Realm = NewRealm;
		}
		UE_LOG(LogWeaponRelay, Log, TEXT("TURN %s challenged (%d), retrying with nonce"), RelayRequestName(Method), ErrorCode);
		SendRequest(Request.Kind, Request.Peer, Request.Channel);
		return;
	}

	UE_LOG(LogWeaponRelay, Error, TEXT("TURN %s rejected: %d %s (peer=%s)"), RelayRequestName(Method), ErrorCode, *ErrorReason,
		Request.Peer.IsValid() ? *Request.Peer->ToString(true) : TEXT("-"));
	if (Request.Kind == ERequestKind::Allocate || Request.Kind == ERequestKind::Refresh)
	{
		EnterFailedState();
	}
}

void FWeaponRelaySocket::HandleAllocateSuccess(const uint8* Data, int32 Size)
{
	int32 Offset = WeaponRelayStun::HeaderSize;
	while (Offset + 4 <= Size)
	{
		const uint16 AttrType = RelayReadU16(Data + Offset);
		const uint16 AttrLen = RelayReadU16(Data + Offset + 2);
		if (Offset + 4 + AttrLen > Size)
		{
			break;
		}
		if (AttrType == WeaponRelayStun::AttrXorRelayedAddress && AttrLen >= 8)
		{
			RelayedAddress = ISocketSubsystem::Get()->CreateInternetAddr();
			RelayDecodeXorAddr(Data + Offset + 4, *RelayedAddress);
			State = EWeaponRelaySocketState::Ready;
			bEverReady = true;
			LastAllocationRefreshTime = FPlatformTime::Seconds();
			UE_LOG(LogWeaponRelay, Log, TEXT("TURN allocated relayed address %s"), *RelayedAddress->ToString(true));
			// 对端也走 relay 时,其数据从 TURN 主机自身的 relayed 端口发出:源 IP 是
			// relayed 公网 IP(或本端连接的 TURN 地址)。预先放行,首包才能到达。
			// 排队中的其它预授权在下一次 Tick 统一发送(时间戳为 0)。
			EnsurePermission(*RelayedAddress);
			EnsurePermission(*TurnServerAddr);
			return;
		}
		Offset += 4 + ((AttrLen + 3) & ~3);
	}

	UE_LOG(LogWeaponRelay, Error, TEXT("Allocate success without XOR-RELAYED-ADDRESS"));
	EnterFailedState();
}

bool FWeaponRelaySocket::ExtractDataIndication(const uint8* Data, int32 Size, uint8* OutData, int32 BufferSize, int32& OutBytes, FInternetAddr& OutPeer)
{
	bool bHasPeer = false;
	bool bHasData = false;
	int32 Offset = WeaponRelayStun::HeaderSize;
	while (Offset + 4 <= Size)
	{
		const uint16 AttrType = RelayReadU16(Data + Offset);
		const uint16 AttrLen = RelayReadU16(Data + Offset + 2);
		if (Offset + 4 + AttrLen > Size)
		{
			break;
		}
		const uint8* AttrData = Data + Offset + 4;
		if (AttrType == WeaponRelayStun::AttrXorPeerAddress && AttrLen >= 8)
		{
			RelayDecodeXorAddr(AttrData, OutPeer);
			bHasPeer = true;
		}
		else if (AttrType == WeaponRelayStun::AttrData && AttrLen <= BufferSize)
		{
			FMemory::Memcpy(OutData, AttrData, AttrLen);
			OutBytes = AttrLen;
			bHasData = true;
		}
		Offset += 4 + ((AttrLen + 3) & ~3);
	}
	return bHasPeer && bHasData;
}

void FWeaponRelaySocket::SendIndication(const uint8* Payload, int32 Count, const FInternetAddr& Peer)
{
	TArray<uint8> Msg;
	RelayWriteHeader(Msg, WeaponRelayStun::MethodSend | WeaponRelayStun::ClassIndication);
	RelayWriteXorAddr(Msg, WeaponRelayStun::AttrXorPeerAddress, Peer);
	RelayWriteAttr(Msg, WeaponRelayStun::AttrData, Payload, Count);
	RelayPatchLength(Msg);
	SendToTurn(Msg.GetData(), Msg.Num());
}

bool FWeaponRelaySocket::HasPendingRelayData(uint32& PendingDataSize)
{
	if (!Underlying)
	{
		return false;
	}
	return Underlying->HasPendingData(PendingDataSize);
}

// ---- FSocket 接口 ----

bool FWeaponRelaySocket::Shutdown(ESocketShutdownMode Mode)
{
	return Underlying ? Underlying->Shutdown(Mode) : false;
}

bool FWeaponRelaySocket::Close()
{
	FScopeLock Lock(&StateLock);
	if (Underlying)
	{
		// 主动释放 allocation(尽力而为,不等响应):否则 relay 端口要等 lifetime
		// 到期才回收,端口段很小时会让后续 Allocate 被拒(508 Insufficient Capacity)。
		if (State == EWeaponRelaySocketState::Ready && TurnServerAddr.IsValid())
		{
			TArray<uint8> Msg;
			FString Key;
			if (BuildRequest(ERequestKind::Deallocate, nullptr, 0, Msg, Key))
			{
				SendToTurn(Msg.GetData(), Msg.Num());
			}
		}
		Underlying->Close();
		delete Underlying;
		Underlying = nullptr;
	}
	State = EWeaponRelaySocketState::Closed;
	PendingRequests.Reset();
	return true;
}

bool FWeaponRelaySocket::Bind(const FInternetAddr& Addr)
{
	FScopeLock Lock(&StateLock);
	if (!Underlying || !Underlying->Bind(Addr))
	{
		return false;
	}
	bUnderlyingBound = true;
	// 底层已绑定 → 发起延迟的 TURN Allocate(若 Allocate 已先被调用而 socket
	// 尚未绑定,事务在此真正启动,避免隐含绑定随机端口与 Bind 冲突)。
	BeginAllocateTransaction();
	return true;
}

bool FWeaponRelaySocket::Connect(const FInternetAddr& Addr)
{
	// Datagram socket:数据经 TURN 服务器转发,无连接语义。
	return true;
}

bool FWeaponRelaySocket::Listen(int32 MaxBacklog)
{
	return true; // datagram: no listen semantics
}

bool FWeaponRelaySocket::WaitForPendingConnection(bool& bHasPendingConnection, const FTimespan& WaitTime)
{
	bHasPendingConnection = false;
	return true;
}

bool FWeaponRelaySocket::HasPendingData(uint32& PendingDataSize)
{
	return HasPendingRelayData(PendingDataSize);
}

class FSocket* FWeaponRelaySocket::Accept(const FString& InSocketDescription)
{
	return nullptr;
}

class FSocket* FWeaponRelaySocket::Accept(FInternetAddr& OutAddr, const FString& InSocketDescription)
{
	return nullptr;
}

bool FWeaponRelaySocket::SendTo(const uint8* Data, int32 Count, int32& BytesSent, const FInternetAddr& Destination)
{
	FScopeLock Lock(&StateLock);
	if (!Underlying)
	{
		return false;
	}

	// 优雅降级 / 直连对端:透传底层 UDP 到真实对端
	if (bFallbackDirect || DirectPeers.Contains(RelayPeerKey(Destination)))
	{
		return Underlying->SendTo(Data, Count, BytesSent, Destination);
	}

	if (State != EWeaponRelaySocketState::Ready)
	{
		// 分配完成前吸收数据包。NetDriver 把 false 视为硬失败(可能级联重连);
		// 短暂的 Allocating 窗口内丢包是正确行为,对端会靠重传恢复。
		BytesSent = Count;
		return true;
	}

	FRelayPeer& Peer = EnsurePeer(Destination);
	if (Peer.bChannelBound)
	{
		// ChannelData: 2 字节 channel + 2 字节长度 + payload(UDP 上无需补齐)
		TArray<uint8> Out;
		Out.Reserve(Count + 4);
		RelayWriteU16(Out, Peer.Channel);
		RelayWriteU16(Out, static_cast<uint16>(Count));
		Out.Append(Data, Count);
		const bool bOk = SendToTurn(Out.GetData(), Out.Num());
		BytesSent = bOk ? Count : 0;
		return bOk;
	}

	// channel 未绑定:Send Indication 立即可用(只需 permission)。
	SendIndication(Data, Count, Destination);
	BytesSent = Count;
	return true;
}

bool FWeaponRelaySocket::RecvFrom(uint8* Data, int32 BufferSize, int32& BytesRead, FInternetAddr& Source, ESocketReceiveFlags::Type Flags)
{
	// RecvFrom 每帧都会被 NetDriver 调用(含 pending 连接阶段的 NetDriver),
	// 借此推进重传与刷新,不依赖外部 Tick 定时器能否拿到正确的 NetDriver。
	Tick();

	if (!Underlying)
	{
		return false;
	}
	if (bFallbackDirect)
	{
		while (Underlying->RecvFrom(Data, BufferSize, BytesRead, Source, Flags))
		{
			if (!EchoConnectProbe(Data, BytesRead, Source))
			{
				return true;
			}
		}
		return false;
	}

	TArray<uint8> Packet;
	Packet.SetNumUninitialized(65536);
	TSharedRef<FInternetAddr> FromAddr = ISocketSubsystem::Get()->CreateInternetAddr();

	// 控制报文就地消化后继续读,直到拿到一个数据包或底层无数据。
	// 上限防止控制报文洪泛时饿死调用方。
	for (int32 Guard = 0; Guard < 32; ++Guard)
	{
		int32 PacketSize = 0;
		if (!Underlying->RecvFrom(Packet.GetData(), Packet.Num(), PacketSize, *FromAddr, Flags))
		{
			const ESocketErrors Error = ISocketSubsystem::Get()->GetLastErrorCode();
			if (Error != SE_EWOULDBLOCK && Error != SE_NO_ERROR)
			{
				UE_LOG(LogWeaponRelay, Verbose, TEXT("Underlying RecvFrom error %d"), static_cast<int32>(Error));
			}
			return false;
		}

		FScopeLock Lock(&StateLock);
		UE_LOG(LogWeaponRelay, VeryVerbose, TEXT("RecvFrom %s: %d bytes, first=0x%04x"),
			*RelayPeerKey(*FromAddr), PacketSize, PacketSize >= 2 ? RelayReadU16(Packet.GetData()) : 0);

		// 非 TURN 服务器来源:直连流量(例如客户端命中 Host 的 LAN/反射候选),
		// 原样上交,并记住该 peer 回包走底层直发。
		if (!IsFromTurnServer(*FromAddr))
		{
			if (PacketSize > BufferSize)
			{
				continue;
			}
			const FString Key = RelayPeerKey(*FromAddr);
			if (!DirectPeers.Contains(Key))
			{
				DirectPeers.Add(Key);
				UE_LOG(LogWeaponRelay, Log, TEXT("Direct UDP peer %s on relay socket"), *Key);
			}
			FMemory::Memcpy(Data, Packet.GetData(), PacketSize);
			BytesRead = PacketSize;
			RelayCopyAddr(Source, *FromAddr);
			if (EchoConnectProbe(Data, BytesRead, Source))
			{
				continue;
			}
			return true;
		}

		if (PacketSize < 4)
		{
			continue;
		}

		const uint16 First = RelayReadU16(Packet.GetData());
		if ((First & WeaponRelayStun::ChannelDataMask) == WeaponRelayStun::ChannelDataMarker)
		{
			const uint16 PayloadLen = RelayReadU16(Packet.GetData() + 2);
			const FString* PeerKey = ChannelToPeer.Find(First);
			const FRelayPeer* Peer = PeerKey ? RelayPeers.Find(*PeerKey) : nullptr;
			if (!Peer || PayloadLen + 4 > PacketSize || PayloadLen > BufferSize)
			{
				continue;
			}
			FMemory::Memcpy(Data, Packet.GetData() + 4, PayloadLen);
			BytesRead = PayloadLen;
			RelayCopyAddr(Source, *Peer->Addr);
			if (EchoConnectProbe(Data, BytesRead, Source))
			{
				continue;
			}
			return true;
		}

		if (PacketSize < WeaponRelayStun::HeaderSize)
		{
			continue;
		}
		const int32 MsgSize = RelayReadU16(Packet.GetData() + 2) + WeaponRelayStun::HeaderSize;
		if (MsgSize > PacketSize || RelayReadU32(Packet.GetData() + 4) != WeaponRelayStun::MagicCookie32)
		{
			continue;
		}

		const uint16 MsgType = RelayReadU16(Packet.GetData());
		if (MsgType == (WeaponRelayStun::MethodData | WeaponRelayStun::ClassIndication))
		{
			TSharedRef<FInternetAddr> PeerAddr = ISocketSubsystem::Get()->CreateInternetAddr();
			int32 PayloadLen = 0;
			if (!ExtractDataIndication(Packet.GetData(), MsgSize, Data, BufferSize, PayloadLen, *PeerAddr))
			{
				continue;
			}
			// 首次来信的对端:建立 channel,后续双向切到 ChannelData。
			EnsurePeer(*PeerAddr);
			BytesRead = PayloadLen;
			RelayCopyAddr(Source, *PeerAddr);
			if (EchoConnectProbe(Data, BytesRead, Source))
			{
				continue;
			}
			return true;
		}

		HandleStunMessage(Packet.GetData(), MsgSize);
	}

	BytesRead = 0;
	return true;
}

bool FWeaponRelaySocket::EchoConnectProbe(const uint8* Data, int32 Size, const FInternetAddr& From)
{
	if (!WeaponConnectProbe::IsProbe(Data, Size, WeaponConnectProbe::TypePing))
	{
		return false;
	}
	// 原路回 pong(直连/relay 由 SendTo 按对端自动选择),客户端据此测 RTT。
	uint64 Sequence = 0;
	double SentTime = 0.0;
	WeaponConnectProbe::Read(Data, Sequence, SentTime);
	uint8 Pong[WeaponConnectProbe::PacketSize];
	WeaponConnectProbe::Write(Pong, WeaponConnectProbe::TypePong, Sequence, SentTime);
	int32 Sent = 0;
	SendTo(Pong, WeaponConnectProbe::PacketSize, Sent, From);
	return true;
}

bool FWeaponRelaySocket::Wait(ESocketWaitConditions::Type Condition, FTimespan WaitTime)
{
	return Underlying ? Underlying->Wait(Condition, WaitTime) : false;
}

ESocketConnectionState FWeaponRelaySocket::GetConnectionState()
{
	// 降级直连时与普通 UDP 一致:底层 socket 决定连接状态
	if (bFallbackDirect)
	{
		return Underlying ? Underlying->GetConnectionState() : SCS_NotConnected;
	}
	return (State == EWeaponRelaySocketState::Ready) ? SCS_Connected : SCS_NotConnected;
}

void FWeaponRelaySocket::GetAddress(FInternetAddr& OutAddr)
{
	FScopeLock Lock(&StateLock);
	// 降级直连:返回底层真实本地地址(而不是 TURN relayed address),
	// 否则 InitListen 会拿服务器端口当本地端口,Host 发布的 endpoint 就是错的
	if (!bFallbackDirect && RelayedAddress.IsValid())
	{
		RelayCopyAddr(OutAddr, *RelayedAddress);
		return;
	}
	if (Underlying)
	{
		Underlying->GetAddress(OutAddr);
	}
}

bool FWeaponRelaySocket::GetPeerAddress(FInternetAddr& OutAddr)
{
	// 多 peer 的 datagram socket 没有单一对端。
	return (bFallbackDirect && Underlying) ? Underlying->GetPeerAddress(OutAddr) : false;
}

bool FWeaponRelaySocket::SetNonBlocking(bool bIsNonBlocking)
{
	bNonBlocking = bIsNonBlocking;
	return Underlying ? Underlying->SetNonBlocking(bIsNonBlocking) : false;
}

bool FWeaponRelaySocket::SetBroadcast(bool bAllowBroadcast)
{
	return Underlying ? Underlying->SetBroadcast(bAllowBroadcast) : false;
}

bool FWeaponRelaySocket::SetNoDelay(bool bIsNoDelay)
{
	return Underlying ? Underlying->SetNoDelay(bIsNoDelay) : false;
}

bool FWeaponRelaySocket::JoinMulticastGroup(const FInternetAddr& GroupAddress)
{
	return false;
}

bool FWeaponRelaySocket::JoinMulticastGroup(const FInternetAddr& GroupAddress, const FInternetAddr& InterfaceAddress)
{
	return false;
}

bool FWeaponRelaySocket::LeaveMulticastGroup(const FInternetAddr& GroupAddress)
{
	return false;
}

bool FWeaponRelaySocket::LeaveMulticastGroup(const FInternetAddr& GroupAddress, const FInternetAddr& InterfaceAddress)
{
	return false;
}

bool FWeaponRelaySocket::SetMulticastLoopback(bool bLoopback)
{
	return false;
}

bool FWeaponRelaySocket::SetMulticastTtl(uint8 TimeToLive)
{
	return false;
}

bool FWeaponRelaySocket::SetMulticastInterface(const FInternetAddr& InterfaceAddress)
{
	return false;
}

bool FWeaponRelaySocket::SetReuseAddr(bool bAllowReuse)
{
	return Underlying ? Underlying->SetReuseAddr(bAllowReuse) : false;
}

bool FWeaponRelaySocket::SetLinger(bool bShouldLinger, int32 Timeout)
{
	return Underlying ? Underlying->SetLinger(bShouldLinger, Timeout) : false;
}

bool FWeaponRelaySocket::SetRecvErr(bool bUseErrorQueue)
{
	return Underlying ? Underlying->SetRecvErr(bUseErrorQueue) : false;
}

bool FWeaponRelaySocket::SetSendBufferSize(int32 Size, int32& OutNewSize)
{
	return Underlying ? Underlying->SetSendBufferSize(Size, OutNewSize) : false;
}

bool FWeaponRelaySocket::SetReceiveBufferSize(int32 Size, int32& OutNewSize)
{
	return Underlying ? Underlying->SetReceiveBufferSize(Size, OutNewSize) : false;
}

int32 FWeaponRelaySocket::GetPortNo()
{
	return Underlying ? Underlying->GetPortNo() : 0;
}
