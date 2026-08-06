// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponPlayerController.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerState.h"
#include "InputMappingContext.h"
#include "LetsDeepRock.h"
#include "Online/WeaponHostMigrationSubsystem.h"
#include "TimerManager.h"
#include "WeaponGameMode.h"

AWeaponPlayerController::AWeaponPlayerController()
{
	// 俯视角鼠标瞄准：显示鼠标光标
	bShowMouseCursor = true;
	DefaultMouseCursor = EMouseCursor::Crosshairs;

	// 鼠标输入直接反映到光标位置（不影响 Controller Rotation）
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;
}

void AWeaponPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// 注册默认的输入映射上下文
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
		ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
		{
			if (CurrentContext)
			{
				Subsystem->AddMappingContext(CurrentContext, 0);
			}
		}
	}
}

void AWeaponPlayerController::PawnLeavingGame()
{
	if (AWeaponGameMode* WeaponGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AWeaponGameMode>() : nullptr;
		WeaponGameMode && WeaponGameMode->PreserveLeavingPlayerAttribution(this))
	{
		return;
	}
	Super::PawnLeavingGame();
}

void AWeaponPlayerController::ClientReceiveMigrationPayloadChunk_Implementation(
	const FUniqueNetIdRepl& ReceiverPlayerId,
	const FWeaponMigrationPayloadEnvelope& Envelope,
	const TArray<uint8>& ChunkBytes)
{
	UWeaponHostMigrationSubsystem* MigrationSubsystem =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>() : nullptr;
	if (!MigrationSubsystem)
	{
		UE_LOG(LogLetsDeepRock, Error, TEXT("Migration chunk received without HostMigrationSubsystem"));
		return;
	}

	bool bAckReady = false;
	FWeaponMigrationPayloadAck Ack;
	FString Error;
	if (!MigrationSubsystem->ReceivePayloadChunk(
		ReceiverPlayerId,
		Envelope,
		ChunkBytes,
		bAckReady,
		Ack,
		Error))
	{
		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("Migration chunk rejected for revision %lld index %d: %s"),
			Envelope.SnapshotRevision,
			Envelope.ChunkIndex,
			*Error);
		return;
	}

	if (bAckReady)
	{
		ServerAcknowledgeMigrationPayload(Ack);
	}
}

void AWeaponPlayerController::ClientUpdateMigrationRoleAssignment_Implementation(
	const FWeaponMigrationRoleAssignment& Assignment)
{
	UWeaponHostMigrationSubsystem* MigrationSubsystem =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>() : nullptr;
	FString Error;
	if (!MigrationSubsystem || !MigrationSubsystem->AcceptRoleAssignment(Assignment, Error))
	{
		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("Migration role assignment rejected for Epoch %llu: %s"),
			Assignment.AuthorityEpoch,
			MigrationSubsystem ? *Error : TEXT("HostMigrationSubsystem is unavailable"));
	}
}

void AWeaponPlayerController::ClientConfirmMigrationCommonCommitted_Implementation(
	const FUniqueNetIdRepl& ReceiverPlayerId,
	const FWeaponMigrationPayloadAck& Ack)
{
	UWeaponHostMigrationSubsystem* MigrationSubsystem =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>() : nullptr;
	FString Error;
	if (!MigrationSubsystem ||
		!MigrationSubsystem->AcceptCommonCommittedAnnouncement(ReceiverPlayerId, Ack, Error))
	{
		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("CommonCommitted announcement rejected for revision %lld: %s"),
			Ack.SnapshotRevision,
			MigrationSubsystem ? *Error : TEXT("HostMigrationSubsystem is unavailable"));
		return;
	}
	const EWeaponMigrationRole ReceiverRole = MigrationSubsystem->GetRoleForPlayer(ReceiverPlayerId);
	if (ReceiverRole == EWeaponMigrationRole::BackupA ||
		ReceiverRole == EWeaponMigrationRole::BackupB)
	{
		ServerConfirmMigrationCommonCommitted(Ack);
	}
}

void AWeaponPlayerController::ClientRequestRecoveryReadyAcknowledgement_Implementation(
	const FGuid MatchId,
	const uint64 AuthorityEpoch,
	const uint64 RosterRevision,
	const FGuid RecoveryAttemptId)
{
	UWeaponHostMigrationSubsystem* MigrationSubsystem =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>() : nullptr;
	FString Error;
	if (!MigrationSubsystem || !MigrationSubsystem->AcceptRecoveryReadyRequest(
		MatchId,
		AuthorityEpoch,
		RosterRevision,
		RecoveryAttemptId,
		Error))
	{
		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("RecoveryReady request rejected for Match %s Epoch %llu Roster %llu Attempt %s: %s"),
			*MatchId.ToString(),
			AuthorityEpoch,
			RosterRevision,
			*RecoveryAttemptId.ToString(),
			MigrationSubsystem ? *Error : TEXT("HostMigrationSubsystem is unavailable"));
		return;
	}

	PendingRecoveryReadyMatchId = MatchId;
	PendingRecoveryReadyEpoch = AuthorityEpoch;
	PendingRecoveryReadyRosterRevision = RosterRevision;
	PendingRecoveryReadyAttemptId = RecoveryAttemptId;
	bRecoveryReadyAcknowledgementPending = true;
	GetWorldTimerManager().SetTimerForNextTick(
		this,
		&AWeaponPlayerController::SendPendingRecoveryReadyAcknowledgement);
}

void AWeaponPlayerController::SendPendingRecoveryReadyAcknowledgement()
{
	if (!bRecoveryReadyAcknowledgementPending)
	{
		return;
	}
	bRecoveryReadyAcknowledgementPending = false;
	ServerAcknowledgeRecoveryReady(
		PendingRecoveryReadyMatchId,
		PendingRecoveryReadyEpoch,
		PendingRecoveryReadyRosterRevision,
		PendingRecoveryReadyAttemptId);
}

void AWeaponPlayerController::ServerAcknowledgeMigrationPayload_Implementation(
	const FWeaponMigrationPayloadAck& Ack)
{
	UWeaponHostMigrationSubsystem* MigrationSubsystem =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>() : nullptr;
	if (!MigrationSubsystem)
	{
		UE_LOG(LogLetsDeepRock, Error, TEXT("Migration ACK received without HostMigrationSubsystem"));
		return;
	}

	FString Error;
	if (!MigrationSubsystem->RecordBackupAcknowledgement(this, Ack, Error))
	{
		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("Migration ACK rejected for revision %lld: %s"),
			Ack.SnapshotRevision,
			*Error);
	}
}

void AWeaponPlayerController::ServerConfirmMigrationCommonCommitted_Implementation(
	const FWeaponMigrationPayloadAck& Ack)
{
	UWeaponHostMigrationSubsystem* MigrationSubsystem =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>() : nullptr;
	FString Error;
	if (!MigrationSubsystem ||
		!MigrationSubsystem->RecordCommonCommittedConfirmation(this, Ack, Error))
	{
		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("CommonCommitted confirmation rejected for revision %lld: %s"),
			Ack.SnapshotRevision,
			MigrationSubsystem ? *Error : TEXT("HostMigrationSubsystem is unavailable"));
	}
}

void AWeaponPlayerController::ServerAcknowledgeRecoveryReady_Implementation(
	const FGuid MatchId,
	const uint64 AuthorityEpoch,
	const uint64 RosterRevision,
	const FGuid RecoveryAttemptId)
{
	UWeaponHostMigrationSubsystem* MigrationSubsystem =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>() : nullptr;
	FString Error;
	if (!MigrationSubsystem || !MigrationSubsystem->RecordRecoveryReadyAcknowledgement(
		this,
		MatchId,
		AuthorityEpoch,
		RosterRevision,
		RecoveryAttemptId,
		Error))
	{
		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("RecoveryReady ACK rejected for Match %s Epoch %llu Roster %llu Attempt %s: %s"),
			*MatchId.ToString(),
			AuthorityEpoch,
			RosterRevision,
			*RecoveryAttemptId.ToString(),
			MigrationSubsystem ? *Error : TEXT("HostMigrationSubsystem is unavailable"));
	}
}
