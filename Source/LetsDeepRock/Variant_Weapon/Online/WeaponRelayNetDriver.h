// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "IpNetDriver.h"
#include "WeaponRelayNetDriver.generated.h"

class FWeaponRelaySocket;

/**
 * Relay 专用 NetDriver:在 UIpNetDriver 之上,把 socket 换成 FWeaponRelaySocket。
 *
 * Gameplay 语义(RPC / Replication / Channel)完全由 UNetDriver 框架承载;
 * 本类只做一件事——让 NetDriver 收发数据时走 TURN 中转而不是裸 UDP。
 *
 * 使用方式(Config/DefaultEngine.ini):
 *   [/Script/LetsDeepRock.WeaponRelayNetDriver]
 *   bUseRelayTransport=true
 */
UCLASS()
class LETSDEEPROCK_API UWeaponRelayNetDriver : public UIpNetDriver
{
	GENERATED_BODY()

public:
	virtual FUniqueSocket CreateSocketForProtocol(const FName& ProtocolType) override;

	/** 连接前由 CentralConnectivity 注入 TURN 服务器与凭证。 */
	void SetRelayConnectionInfo(const FString& InTurnServerAddr, const FString& InUsername, const FString& InPassword);

	/** 供外部轮询推进 TURN 事务(每帧调用)。 */
	void TickRelay();

	/** TURN 分配失败已降级为直连时返回 true。 */
	bool IsRelayFallbackDirect();

private:
	FString TurnServerAddr;
	FString TurnUsername;
	FString TurnPassword;
};
