// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IpNetDriver.h"
#include "WeaponRelayNetDriver.generated.h"

class FWeaponRelaySocket;

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

	/** 供外部轮询推进 TURN 事务(每帧调用)。 */
	void TickRelay();

	/** TURN 分配失败已降级为直连时返回 true。 */
	bool IsRelayFallbackDirect();

	/** 已分配的 relayed 地址(host:port 字符串);未分配时返回空。 */
	FString GetRelayedAddressString() const;

	/** 本连接是否打算走 relay(socket 创建前即可查询)。 */
	bool ShouldUseRelayTransport() const;

private:
	bool ResolveRelayCredentials(FString& OutTurnServerAddr, FString& OutUsername, FString& OutPassword);

	FString TurnServerAddr;
	FString TurnUsername;
	FString TurnPassword;
	bool bCredentialsExplicit = false;
	bool bCredentialsResolved = false;
};
