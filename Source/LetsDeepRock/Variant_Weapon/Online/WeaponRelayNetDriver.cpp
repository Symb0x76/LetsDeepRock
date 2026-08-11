// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponRelayNetDriver.h"
#include "WeaponRelaySocket.h"
#include "SocketSubsystem.h"

DEFINE_LOG_CATEGORY_STATIC(LogWeaponRelayDriver, Log, All);

FUniqueSocket UWeaponRelayNetDriver::CreateSocketForProtocol(const FName& ProtocolType)
{
	// Create the underlying UDP socket exactly like UIpNetDriver would, then
	// wrap it in the relay socket so all data flows through the TURN server.
	FUniqueSocket Underlying = Super::CreateSocketForProtocol(ProtocolType);
	if (!Underlying.IsValid())
	{
		return nullptr;
	}

	FSocket* RawSocket = Underlying.Release();
	FWeaponRelaySocket* RelaySocket = new FWeaponRelaySocket(RawSocket, TEXT("WeaponRelay"));

	if (!TurnServerAddr.IsEmpty())
	{
		RelaySocket->Allocate(TurnServerAddr, TurnUsername, TurnPassword);
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
}

void UWeaponRelayNetDriver::TickRelay()
{
	if (FWeaponRelaySocket* Relay = static_cast<FWeaponRelaySocket*>(GetSocket()))
	{
		Relay->Tick();
	}
}
