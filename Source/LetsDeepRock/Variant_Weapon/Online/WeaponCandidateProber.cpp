// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponCandidateProber.h"
#include "WeaponConnectProbe.h"
#include "WeaponRelaySocket.h"
#include "IPAddress.h"
#include "Sockets.h"
#include "SocketSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogWeaponCandidateProber, Log, All);

namespace WeaponCandidateProberConfig
{
	// 每轮 ping 间隔:单包丢失不至于把可达候选判成不可达
	constexpr double PingIntervalSeconds = 0.1;
}

FWeaponCandidateProber::~FWeaponCandidateProber()
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (DirectSocket && Sockets)
	{
		Sockets->DestroySocket(DirectSocket);
	}
	// Close 内会发 Refresh(LIFETIME=0) 立即释放测速用的 TURN 端口
	delete RelaySocket;
}

TSharedPtr<FInternetAddr> FWeaponCandidateProber::MakeAddr(const FString& Address, int32 Port) const
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TSharedRef<FInternetAddr> Addr = Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
	bool bValid = false;
	Addr->SetIp(*Address, bValid);
	Addr->SetPort(Port);
	return bValid ? TSharedPtr<FInternetAddr>(Addr) : nullptr;
}

void FWeaponCandidateProber::AddDirectTarget(int32 QueueIndex, const FString& Address, int32 Port)
{
	if (TSharedPtr<FInternetAddr> Addr = MakeAddr(Address, Port))
	{
		FTarget& Target = Targets.AddDefaulted_GetRef();
		Target.QueueIndex = QueueIndex;
		Target.Addr = Addr;
	}
}

void FWeaponCandidateProber::AddRelayTarget(int32 QueueIndex, const FString& Address, int32 Port,
	const FString& TurnServerAddr, const FString& Username, const FString& Password)
{
	if (TSharedPtr<FInternetAddr> Addr = MakeAddr(Address, Port))
	{
		FTarget& Target = Targets.AddDefaulted_GetRef();
		Target.QueueIndex = QueueIndex;
		Target.Addr = Addr;
		Target.bRelay = true;
		TurnAddr = TurnServerAddr;
		TurnUser = Username;
		TurnPass = Password;
	}
}

bool FWeaponCandidateProber::Start(double InWindowSeconds)
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!Sockets || Targets.Num() == 0)
	{
		return false;
	}
	WindowSeconds = InWindowSeconds;
	TSharedRef<FInternetAddr> Any = Sockets->CreateInternetAddr(FNetworkProtocolTypes::IPv4);
	Any->SetAnyAddress();

	const bool bHasDirect = Targets.ContainsByPredicate([](const FTarget& T) { return !T.bRelay; });
	if (bHasDirect)
	{
		DirectSocket = Sockets->CreateSocket(NAME_DGram, TEXT("CandidateProbeDirect"), FNetworkProtocolTypes::IPv4);
		if (DirectSocket)
		{
			DirectSocket->SetNonBlocking(true);
			DirectSocket->Bind(*Any);
		}
	}

	if (!TurnAddr.IsEmpty())
	{
		if (FSocket* Raw = Sockets->CreateSocket(NAME_DGram, TEXT("CandidateProbeRelayUnderlying"), FNetworkProtocolTypes::IPv4))
		{
			Raw->SetNonBlocking(true);
			RelaySocket = new FWeaponRelaySocket(Raw, TEXT("WeaponRelay"));
			RelaySocket->Allocate(TurnAddr, TurnUser, TurnPass);
			RelaySocket->Bind(*Any);
		}
	}

	StartTime = FPlatformTime::Seconds();
	LastPingTime = 0.0;
	return DirectSocket || RelaySocket;
}

void FWeaponCandidateProber::SendPings(double Now)
{
	for (int32 Index = 0; Index < Targets.Num(); ++Index)
	{
		FTarget& Target = Targets[Index];
		if (Target.RttSeconds >= 0.0)
		{
			continue;
		}
		FSocket* Socket = Target.bRelay ? static_cast<FSocket*>(RelaySocket) : DirectSocket;
		if (!Socket)
		{
			continue;
		}
		// relay socket 在 Allocate 完成前会吸收数据包;完成后的下一轮自然发出
		uint8 Ping[WeaponConnectProbe::PacketSize];
		WeaponConnectProbe::Write(Ping, WeaponConnectProbe::TypePing, static_cast<uint64>(Index), Now);
		int32 Sent = 0;
		Socket->SendTo(Ping, WeaponConnectProbe::PacketSize, Sent, *Target.Addr);
	}
}

void FWeaponCandidateProber::Drain(FSocket& Socket)
{
	ISocketSubsystem* Sockets = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	TSharedRef<FInternetAddr> From = Sockets->CreateInternetAddr();
	uint8 Buffer[2048];
	int32 Read = 0;
	while (Socket.RecvFrom(Buffer, sizeof(Buffer), Read, *From))
	{
		if (!WeaponConnectProbe::IsProbe(Buffer, Read, WeaponConnectProbe::TypePong))
		{
			continue;
		}
		uint64 Sequence = 0;
		double SentTime = 0.0;
		WeaponConnectProbe::Read(Buffer, Sequence, SentTime);
		const int32 Index = static_cast<int32>(Sequence);
		if (Targets.IsValidIndex(Index) && Targets[Index].RttSeconds < 0.0)
		{
			FTarget& Target = Targets[Index];
			Target.RttSeconds = FPlatformTime::Seconds() - SentTime;
			UE_LOG(LogWeaponCandidateProber, Log, TEXT("Candidate %s (%s) answered in %.1f ms"),
				*Target.Addr->ToString(true), Target.bRelay ? TEXT("relay") : TEXT("direct"), Target.RttSeconds * 1000.0);
		}
	}
}

bool FWeaponCandidateProber::Tick()
{
	const double Now = FPlatformTime::Seconds();
	if (Now - LastPingTime >= WeaponCandidateProberConfig::PingIntervalSeconds)
	{
		LastPingTime = Now;
		SendPings(Now);
	}
	if (DirectSocket)
	{
		Drain(*DirectSocket);
	}
	if (RelaySocket)
	{
		Drain(*RelaySocket); // 同时驱动 TURN 事务(RecvFrom 内 Tick)
	}

	const bool bAllAnswered = !Targets.ContainsByPredicate([](const FTarget& T) { return T.RttSeconds < 0.0; });
	return bAllAnswered || Now - StartTime >= WindowSeconds;
}
