// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponRelayNetDriver.h"
#include "WeaponRelaySocket.h"
#include "WeaponCentralConnectivitySubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "SocketSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogWeaponRelayDriver, Log, All);

FUniqueSocket UWeaponRelayNetDriver::CreateSocketForProtocol(const FName& ProtocolType)
{
	// Create the underlying UDP socket exactly like UIpNetDriver would.
	FUniqueSocket Underlying = Super::CreateSocketForProtocol(ProtocolType);
	if (!Underlying.IsValid())
	{
		return nullptr;
	}

	// 只有需要 relay 时才包装 socket;否则原样透传(等价 IpNetDriver)。
	if (!ShouldUseRelayTransport())
	{
		return Underlying;
	}

	// 解析 TURN 凭证(显式注入或从 CentralConnectivity 子系统拉取)
	FString TurnAddr, TurnUser, TurnPass;
	if (!ResolveRelayCredentials(TurnAddr, TurnUser, TurnPass))
	{
		UE_LOG(LogWeaponRelayDriver, Warning,
			TEXT("Relay transport requested but no TURN credentials available; falling back to direct UDP"));
		return Underlying;
	}

	FSocket* RawSocket = Underlying.Release();
	FWeaponRelaySocket* RelaySocket = new FWeaponRelaySocket(RawSocket, TEXT("WeaponRelay"));

	if (!TurnAddr.IsEmpty())
	{
		RelaySocket->Allocate(TurnAddr, TurnUser, TurnPass);
		UE_LOG(LogWeaponRelayDriver, Log, TEXT("Relay socket Allocate to %s"), *TurnAddr);
	}

	// FUniqueSocket uses FSocketDeleter (calls ISocketSubsystem::DestroySocket);
	// the underlying socket must be destroyed via the subsystem, matching
	// ISocketSubsystem::CreateUniqueSocket's ownership model.
	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get();
	return FUniqueSocket(RelaySocket, FSocketDeleter(SocketSubsystem));
}

void UWeaponRelayNetDriver::SetRelayConnectionInfo(const FString& InTurnServerAddr, const FString& InUsername, const FString& InPassword)
{
	TurnServerAddr = InTurnServerAddr;
	TurnUsername = InUsername;
	TurnPassword = InPassword;
	bCredentialsExplicit = true;
}

bool UWeaponRelayNetDriver::ShouldUseRelayTransport() const
{
	if (bCredentialsExplicit && !TurnServerAddr.IsEmpty())
	{
		return true;
	}

	// 无显式凭证:仅当客户端已明确选择 relay 回退(无可用 direct 候选)
	// 且已缓存 TURN 凭证时才走 relay。有 direct 候选时保持裸 UDP 直连。
	UWorld* DriverWorld = GetWorld();
	UGameInstance* GI = DriverWorld ? DriverWorld->GetGameInstance() : nullptr;
	if (GI)
	{
		if (UWeaponCentralConnectivitySubsystem* Central = GI->GetSubsystem<UWeaponCentralConnectivitySubsystem>())
		{
			return Central->HasRelayCredentials() && Central->ShouldUseRelayFallback();
		}
	}
	return false;
}

bool UWeaponRelayNetDriver::ResolveRelayCredentials(FString& OutTurnServerAddr, FString& OutUsername, FString& OutPassword)
{
	if (bCredentialsExplicit)
	{
		OutTurnServerAddr = TurnServerAddr;
		OutUsername = TurnUsername;
		OutPassword = TurnPassword;
		return !OutTurnServerAddr.IsEmpty();
	}

	// 无显式凭证:从 CentralConnectivity 子系统拉取(客户端 join 后已预取缓存)。
	UWorld* DriverWorld = GetWorld();
	UGameInstance* GI = DriverWorld ? DriverWorld->GetGameInstance() : nullptr;
	if (!GI)
	{
		return false;
	}
	UWeaponCentralConnectivitySubsystem* Central = GI->GetSubsystem<UWeaponCentralConnectivitySubsystem>();
	if (!Central)
	{
		return false;
	}

	if (!Central->HasRelayCredentials())
	{
		return false;
	}
	FString Error;
	if (!Central->ConfigureRelayNetDriver(Error))
	{
		UE_LOG(LogWeaponRelayDriver, Warning, TEXT("Cannot obtain relay credentials: %s"), *Error);
		return false;
	}

	OutTurnServerAddr = TurnServerAddr;
	OutUsername = TurnUsername;
	OutPassword = TurnPassword;
	return !OutTurnServerAddr.IsEmpty();
}

void UWeaponRelayNetDriver::TickRelay()
{
	if (FWeaponRelaySocket* Relay = static_cast<FWeaponRelaySocket*>(GetSocket()))
	{
		Relay->Tick();
	}
}

bool UWeaponRelayNetDriver::IsRelayFallbackDirect()
{
	if (FWeaponRelaySocket* Relay = static_cast<FWeaponRelaySocket*>(GetSocket()))
	{
		return Relay->IsFallbackDirect();
	}
	return false;
}

FString UWeaponRelayNetDriver::GetRelayedAddressString() const
{
	UWeaponRelayNetDriver* Self = const_cast<UWeaponRelayNetDriver*>(this);
	if (FWeaponRelaySocket* Relay = static_cast<FWeaponRelaySocket*>(Self->GetSocket()))
	{
		if (const FInternetAddr* Addr = Relay->GetRelayedAddress())
		{
			return FString::Printf(TEXT("%s:%d"), *Addr->ToString(false), Addr->GetPort());
		}
	}
	return FString();
}
