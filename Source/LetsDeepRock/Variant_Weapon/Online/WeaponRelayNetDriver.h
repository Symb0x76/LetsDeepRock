// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IpNetDriver.h"
#include "Sockets.h"
#include "WeaponRelayNetDriver.generated.h"

class FWeaponRelaySocket;
class UWeaponCentralConnectivitySubsystem;

/**
 * Relay 感知 NetDriver:在 UIpNetDriver 之上,把 socket 换成 FWeaponRelaySocket。
 *
 * 作为 GameNetDriver 的覆盖类被引擎实例化,本类做两件事:
 *  - 默认(无 relay 上下文):行为与 UIpNetDriver 完全一致(透传);
 *  - 当 CentralConnectivity 已缓存 TURN 凭证(客户端 fallback 或 Host 中继)时,
 *    把 socket 包装为 FWeaponRelaySocket,让数据走 TURN 中转。
 *
 * 凭证从 UWeaponCentralConnectivitySubsystem 拉取,不依赖外部注入时机
 * (CreateSocketForProtocol 时子系统必然已初始化)。
 */
UCLASS()
class LETSDEEPROCK_API UWeaponRelayNetDriver : public UIpNetDriver
{
	GENERATED_BODY()

public:
	virtual FUniqueSocket CreateSocketForProtocol(const FName& ProtocolType) override;

	/** 显式注入 TURN 服务器与凭证(优先于子系统拉取)。 */
	void SetRelayConnectionInfo(const FString& InTurnServerAddr, const FString& InUsername, const FString& InPassword);

	/**
	 * 把当前已绑定的 socket 热升级为 relay 包装(socket 创建后凭证才到达时用)。
	 * 仅 Host(监听)场景有效;客户端 socket 创建时已能读到凭证缓存。
	 * @return 升级成功或本就已包装时为 true
	 */
	bool UpgradeSocketToRelay();

	void TickRelay();
	
	bool IsRelayFallbackDirect();
	
	FString GetRelayedAddressString() const;
	
	bool ShouldUseRelayTransport() const;
	
	bool IsSocketRelay()
	{
		const FSocket* Sock = GetSocket();
		return Sock && Sock->GetDescription() == TEXT("WeaponRelay");
	}

private:
	/** 当前 socket 为 relay 包装时返回它,否则 nullptr。 */
	FWeaponRelaySocket* GetRelaySocket() const;

	bool ResolveRelayCredentials(FString& OutTurnServerAddr, FString& OutUsername, FString& OutPassword);

	// 解析 Central 子系统:先走 World(pending 阶段 GetWorld 为 null 时),
	// 回退到 GEngine 按 pending net driver 查 WorldContext → OwningGameInstance。
	UWeaponCentralConnectivitySubsystem* GetCentralConnectivitySubsystem() const;

	FString TurnServerAddr;
	FString TurnUsername;
	FString TurnPassword;
	bool bCredentialsExplicit = false;
	bool bCredentialsResolved = false;
};
