// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "Online/WeaponHostMigrationTypes.h"
#include "WeaponPlayerController.generated.h"

class UInputMappingContext;

/**
 *  俯视角武器系统 Player Controller（Variant_Weapon 骨架）
 *
 *  阶段 0：仅负责注册 Enhanced Input 的 IMC。
 *  后续阶段将扩展：武器输入 InputTag 分发、死亡重生等。
 *
 *  3C 优化：启用鼠标光标显示，角色始终面向鼠标位置。
 */
UCLASS(abstract, Config = "Game")
class AWeaponPlayerController : public APlayerController
{
	GENERATED_BODY()

public:

	AWeaponPlayerController();

	/**
	 * Server -> owning Backup: one paced chunk of an immutable migration payload.
	 * ReceiverPlayerId is supplied by authority because PlayerState identity replication may lag this RPC.
	 */
	UFUNCTION(Client, Reliable)
	void ClientReceiveMigrationPayloadChunk(
		const FUniqueNetIdRepl& ReceiverPlayerId,
		const FWeaponMigrationPayloadEnvelope& Envelope,
		const TArray<uint8>& ChunkBytes);

	/** Server -> every client: current fixed roles and authority fencing term. */
	UFUNCTION(Client, Reliable)
	void ClientUpdateMigrationRoleAssignment(const FWeaponMigrationRoleAssignment& Assignment);

	/**
	 * Server -> every client: A and B both ACKed this exact recovery payload.
	 * ReceiverPlayerId keeps role resolution independent of local PlayerState replication timing.
	 */
	UFUNCTION(Client, Reliable)
	void ClientConfirmMigrationCommonCommitted(
		const FUniqueNetIdRepl& ReceiverPlayerId,
		const FWeaponMigrationPayloadAck& Ack);

	/** Candidate -> owning survivor: the restored World is ready for presentation acknowledgement. */
	UFUNCTION(Client, Reliable)
	void ClientRequestRecoveryReadyAcknowledgement(
		FGuid MatchId,
		uint64 AuthorityEpoch,
		uint64 RosterRevision,
		FGuid RecoveryAttemptId);

protected:

	/** 默认输入映射上下文 */
	UPROPERTY(EditAnywhere, Category = "Input|Input Mappings")
	TArray<UInputMappingContext*> DefaultMappingContexts;

protected:

	/** 初始化输入绑定 */
	virtual void SetupInputComponent() override;

	/** Preserve an inert attribution source before the engine's default Pawn destruction. */
	virtual void PawnLeavingGame() override;

	/** Owning Backup -> Host: emitted only after local Pending promotion succeeds. */
	UFUNCTION(Server, Reliable)
	void ServerAcknowledgeMigrationPayload(const FWeaponMigrationPayloadAck& Ack);

	/** Backup -> Host: confirms the announced CommonCommitted payload is pinned locally. */
	UFUNCTION(Server, Reliable)
	void ServerConfirmMigrationCommonCommitted(const FWeaponMigrationPayloadAck& Ack);

	UFUNCTION(Server, Reliable)
	void ServerAcknowledgeRecoveryReady(
		FGuid MatchId,
		uint64 AuthorityEpoch,
		uint64 RosterRevision,
		FGuid RecoveryAttemptId);

	void SendPendingRecoveryReadyAcknowledgement();

	FGuid PendingRecoveryReadyMatchId;
	FGuid PendingRecoveryReadyAttemptId;
	uint64 PendingRecoveryReadyEpoch = 0;
	uint64 PendingRecoveryReadyRosterRevision = 0;
	bool bRecoveryReadyAcknowledgementPending = false;
};
