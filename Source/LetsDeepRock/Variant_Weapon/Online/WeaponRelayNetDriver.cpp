// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponRelayNetDriver.h"
#include "WeaponRelaySocket.h"
#include "WeaponCentralConnectivitySubsystem.h"
#include "Engine/Engine.h"
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

	// FWeaponRelaySocket::Allocate 延迟 TURN 事务到 Bind 后发起。热升级路径
	// 无后续 InitBase 的 InitBindSockets,必须在此显式 Bind(0.0.0.0:0),
	// Bind 内部会触发延迟的 Allocate 事务,且避免隐含绑定与 Bind 冲突。
	FWeaponRelaySocket* RelaySocket = new FWeaponRelaySocket(NewUnderlying.Release(), TEXT("WeaponRelay"));
	TSharedRef<FInternetAddr> BindAddr = SocketSubsystem->CreateInternetAddr();
	BindAddr->SetAnyAddress();
	RelaySocket->Allocate(TurnServerAddr, TurnUsername, TurnPassword);
	RelaySocket->Bind(*BindAddr);
	UE_LOG(LogWeaponRelayDriver, Log, TEXT("Upgraded socket to relay, Allocate to %s"), *TurnServerAddr);

	// 安装到 netdriver(传播到 NetConnection 与接收线程;旧 socket 延迟清理)。
	SetSocketAndLocalAddress(TSharedPtr<FSocket>(RelaySocket));
	return true;
}

bool UWeaponRelayNetDriver::ShouldUseRelayTransport() const
{
	if (bCredentialsExplicit && !TurnServerAddr.IsEmpty())
	{
		UE_LOG(LogWeaponRelayDriver, Log, TEXT("ShouldUseRelayTransport: explicit creds -> relay"));
		return true;
	}

	// 拿 Central 子系统:优先走 World(pending 阶段 GetWorld 为 null 时),
	// 回退到 WorldContext(pending net driver 也注册了 WorldContext)。
	UWeaponCentralConnectivitySubsystem* Central = GetCentralConnectivitySubsystem();
	if (!Central)
	{
		UE_LOG(LogWeaponRelayDriver, Warning,
			TEXT("ShouldUseRelayTransport: no Central subsystem -> direct"));
		return false;
	}
	if (!Central->HasRelayCredentials())
	{
		UE_LOG(LogWeaponRelayDriver, Warning,
			TEXT("ShouldUseRelayTransport: no relay credentials -> direct"));
		return false;
	}

	// Host(监听服务器):凭证已缓存即走 relay,不依赖 bCredentialsExplicit
	// 的注入时序(凭证异步到达,而 Listen socket 在 travel 时已创建)。
	UWorld* DriverWorld = GetWorld();
	if (DriverWorld && DriverWorld->GetNetMode() == NM_ListenServer)
	{
		UE_LOG(LogWeaponRelayDriver, Log, TEXT("ShouldUseRelayTransport: listen host -> relay"));
		return true;
	}

	// 客户端:仅当已明确选择 relay 回退(无可用 direct 候选)才走 relay。
	// 有 direct 候选时保持裸 UDP 直连。
	const bool bUseRelay = Central->ShouldUseRelayFallback();
	UE_LOG(LogWeaponRelayDriver, Log,
		TEXT("ShouldUseRelayTransport: client fallback=%d -> %s"),
		bUseRelay ? 1 : 0, bUseRelay ? TEXT("relay") : TEXT("direct"));
	return bUseRelay;
}

UWeaponCentralConnectivitySubsystem* UWeaponRelayNetDriver::GetCentralConnectivitySubsystem() const
{
	UWorld* DriverWorld = GetWorld();
	UGameInstance* GI = DriverWorld ? DriverWorld->GetGameInstance() : nullptr;
	if (GI)
	{
		return GI->GetSubsystem<UWeaponCentralConnectivitySubsystem>();
	}

	// pending 阶段 GetWorld 为 null:经 GEngine 拿该 pending driver 注册的
	// WorldContext,再解析其 OwningGameInstance 的 Central 子系统。
	if (GEngine)
	{
		const FWorldContext* WorldContext =
			GEngine->GetWorldContextFromPendingNetGameNetDriver(this);
		if (WorldContext && WorldContext->OwningGameInstance)
		{
			return WorldContext->OwningGameInstance->GetSubsystem<UWeaponCentralConnectivitySubsystem>();
		}
	}
	return nullptr;
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
	// 用 GetCentralConnectivitySubsystem 兼容 pending 阶段 GetWorld 为 null;
	// 直接读凭证缓存,不走 ConfigureRelayNetDriver(那是 Host 升级 socket 用的,
	// pending 阶段无 active NetDriver 必然失败)。
	UWeaponCentralConnectivitySubsystem* Central = GetCentralConnectivitySubsystem();
	if (!Central)
	{
		return false;
	}

	if (!Central->GetCachedRelayCredentials(OutTurnServerAddr, OutUsername, OutPassword))
	{
		UE_LOG(LogWeaponRelayDriver, Warning,
			TEXT("Cannot obtain relay credentials: no cached credentials"));
		return false;
	}
	return !OutTurnServerAddr.IsEmpty();
}

FWeaponRelaySocket* UWeaponRelayNetDriver::GetRelaySocket() const
{
	// 纯 UDP 直连时 socket 不是 FWeaponRelaySocket,不能盲目 static_cast。
	FSocket* Sock = const_cast<UWeaponRelayNetDriver*>(this)->GetSocket();
	return (Sock && Sock->GetDescription() == TEXT("WeaponRelay")) ? static_cast<FWeaponRelaySocket*>(Sock) : nullptr;
}

void UWeaponRelayNetDriver::TickRelay()
{
	if (FWeaponRelaySocket* Relay = GetRelaySocket())
	{
		Relay->Tick();
	}
}

bool UWeaponRelayNetDriver::IsRelayFallbackDirect()
{
	if (FWeaponRelaySocket* Relay = GetRelaySocket())
	{
		return Relay->IsFallbackDirect();
	}
	return false;
}

FString UWeaponRelayNetDriver::GetRelayedAddressString() const
{
	if (FWeaponRelaySocket* Relay = GetRelaySocket())
	{
		if (const FInternetAddr* Addr = Relay->GetRelayedAddress())
		{
			return FString::Printf(TEXT("%s:%d"), *Addr->ToString(false), Addr->GetPort());
		}
	}
	return FString();
}
