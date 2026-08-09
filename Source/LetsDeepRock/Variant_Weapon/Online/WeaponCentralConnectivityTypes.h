// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "WeaponCentralConnectivityTypes.generated.h"

/** 与 Go Coordinator 共享的协议版本;不匹配直接拒绝。 */
UENUM()
enum class EWeaponCentralConnectivityState : uint8
{
	Disconnected,
	Authenticating,
	Connected,
	GatheringCandidates,
	Checking,
	DirectConnected,
	Relayed,
	Failed,
	Expired,
};

/** 中心控制通道的登录/连接状态机。 */
UENUM()
enum class EWeaponCentralAuthState : uint8
{
	None,
	ChallengeRequested,
	ChallengeSigned,
	Registered,
	SessionRequested,
	Authenticated,
	Failed,
};

/** Go Coordinator /v1/... 返回的 endpoint candidate 的 UE 侧结构。 */
USTRUCT()
struct FWeaponCentralEndpointCandidate
{
	GENERATED_BODY()

	UPROPERTY()
	FString MatchId;

	UPROPERTY()
	uint64 AuthorityEpoch = 0;

	UPROPERTY()
	uint64 RosterRevision = 0;

	UPROPERTY()
	FString PeerId;

	UPROPERTY()
	FString CandidateId;

	UPROPERTY()
	FString Transport;      // "UDP" | "TCP" | "Relay"

	UPROPERTY()
	FString CandidateType;  // "Host" | "ServerReflexive" | "Relayed"

	UPROPERTY()
	FString Address;

	UPROPERTY()
	int32 Port = 0;

	UPROPERTY()
	uint32 Priority = 0;

	UPROPERTY()
	FString Foundation;

	UPROPERTY()
	FString RelayAllocationId;
};

/** 中心授予的 Host Lease:复用 WeaponHostMigrationTypes.h 的 FWeaponCentralHostLease。
 *  此处不再重复定义;wire 转换在 Subsystem 内完成。 */
