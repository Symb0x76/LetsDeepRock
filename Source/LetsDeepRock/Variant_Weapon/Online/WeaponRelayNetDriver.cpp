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

bool UWeaponRelayNetDriver::UpgradeSocketToRelay()
{
	if (!GetSocket())
	{
		return false;
	}

	// 已包装(socket 创建时凭证已就绪)则无需升级。
	if (GetSocket()->GetDescription() == TEXT("WeaponRelay"))
	{
		return true;
	}

	if (TurnServerAddr.IsEmpty())
	{
		UE_LOG(LogWeaponRelayDriver, Warning,
			TEXT("UpgradeSocketToRelay: no TURN server configured; relay transport disabled"));
		return false;
	}

	// 创建全新底层 socket 并包装。旧 socket 由 netdriver 正常释放
	// (SetSocketAndLocalAddress 触发 safe/deferred cleanup),无所有权冲突。
	// Host 的 TURN 分配地址由 TURN 服务器决定,与本地 listen 端口无关,
	// 客户端经 relay 走的是 relayed 地址,不直接连本地端口。
	ISocketSubsystem* SocketSubsystem = GetSocketSubsystem();
	if (!SocketSubsystem)
	{
		return false;
	}
	FUniqueSocket NewUnderlying = SocketSubsystem->CreateUniqueSocket(NAME_DGram, TEXT("WeaponRelayUnderlying"));
	if (!NewUnderlying.IsValid())
	{
		return false;
	}
	// CreateUniqueSocket 返回阻塞模式 socket;UIpNetDriver::InitBase 原本会给
	// socket 设非阻塞,但热升级新建的 socket 绕过了 InitBase。阻塞的 RecvFrom
	// 会在主线程 TickDispatch 里无限等待(无入站包时),导致 UI 卡死。
	NewUnderlying->SetNonBlocking(true);

	FWeaponRelaySocket* RelaySocket = new FWeaponRelaySocket(NewUnderlying.Release(), TEXT("WeaponRelay"));
	RelaySocket->Allocate(TurnServerAddr, TurnUsername, TurnPassword);
	UE_LOG(LogWeaponRelayDriver, Log, TEXT("Upgraded socket to relay, Allocate to %s"), *TurnServerAddr);

	// 安装到 netdriver(传播到 NetConnection 与接收线程;旧 socket 延迟清理)。
	SetSocketAndLocalAddress(TSharedPtr<FSocket>(RelaySocket));
	return true;
}

bool UWeaponRelayNetDriver::ShouldUseRelayTransport() const
{
	if (bCredentialsExplicit && !TurnServerAddr.IsEmpty())
	{
		return true;
	}

	UWorld* DriverWorld = GetWorld();
	UGameInstance* GI = DriverWorld ? DriverWorld->GetGameInstance() : nullptr;
	if (!GI)
	{
		return false;
	}
	UWeaponCentralConnectivitySubsystem* Central = GI->GetSubsystem<UWeaponCentralConnectivitySubsystem>();
	if (!Central || !Central->HasRelayCredentials())
	{
		return false;
	}

	// Host(监听服务器):凭证已缓存即走 relay,不依赖 bCredentialsExplicit
	// 的注入时序(凭证异步到达,而 Listen socket 在 travel 时已创建)。
	if (DriverWorld->GetNetMode() == NM_ListenServer)
	{
		return true;
	}

	// 客户端:仅当已明确选择 relay 回退(无可用 direct 候选)才走 relay。
	// 有 direct 候选时保持裸 UDP 直连。
	return Central->ShouldUseRelayFallback();
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
