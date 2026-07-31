// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponHostMigrationSubsystem.h"
#include "LetsDeepRock.h"
#include "WeaponMatchSaveGame.h"
#include "WeaponSnapshot.h"
#include "WeaponSnapshotCoordinator.h"
#include "WeaponPlayerController.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "HAL/PlatformMisc.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"

bool FWeaponMigrationRoleMember::IdentifiesSameMember(
	const FWeaponMigrationRoleMember& Other) const
{
	return PlayerId == Other.PlayerId && OriginalSlot == Other.OriginalSlot;
}

EWeaponMigrationRole FWeaponMigrationRoleAssignment::ResolveRole(
	const FUniqueNetIdRepl& PlayerId) const
{
	if (!PlayerId.IsValid())
	{
		return EWeaponMigrationRole::None;
	}
	if (ActiveHost.PlayerId == PlayerId)
	{
		return EWeaponMigrationRole::ActiveHost;
	}
	if (BackupA.PlayerId == PlayerId)
	{
		return EWeaponMigrationRole::BackupA;
	}
	if (BackupB.PlayerId == PlayerId)
	{
		return EWeaponMigrationRole::BackupB;
	}
	return EWeaponMigrationRole::None;
}

bool FWeaponMigrationRoleAssignment::IdentifiesSameAssignment(
	const FWeaponMigrationRoleAssignment& Other) const
{
	return MatchId == Other.MatchId &&
		AuthorityEpoch == Other.AuthorityEpoch &&
		RosterRevision == Other.RosterRevision &&
		ActiveHost.IdentifiesSameMember(Other.ActiveHost) &&
		BackupA.IdentifiesSameMember(Other.BackupA) &&
		BackupB.IdentifiesSameMember(Other.BackupB);
}

bool FWeaponMigrationPayloadEnvelope::IdentifiesSamePayload(
	const FWeaponMigrationPayloadEnvelope& Other) const
{
	return ProtocolVersion == Other.ProtocolVersion &&
		MatchId == Other.MatchId &&
		AuthorityEpoch == Other.AuthorityEpoch &&
		SnapshotRevision == Other.SnapshotRevision &&
		RosterRevision == Other.RosterRevision &&
		SaveFormatVersion == Other.SaveFormatVersion &&
		SnapshotSchemaVersion == Other.SnapshotSchemaVersion &&
		PayloadByteCount == Other.PayloadByteCount &&
		ChunkCount == Other.ChunkCount &&
		PayloadHash == Other.PayloadHash;
}

bool FWeaponMigrationPayloadAck::IdentifiesPayload(
	const FWeaponMigrationPayloadEnvelope& Envelope) const
{
	return MatchId == Envelope.MatchId &&
		AuthorityEpoch == Envelope.AuthorityEpoch &&
		SnapshotRevision == Envelope.SnapshotRevision &&
		RosterRevision == Envelope.RosterRevision &&
		PayloadHash == Envelope.PayloadHash;
}

bool FWeaponMigrationPayloadAck::IdentifiesSamePayload(
	const FWeaponMigrationPayloadAck& Other) const
{
	return MatchId == Other.MatchId &&
		AuthorityEpoch == Other.AuthorityEpoch &&
		SnapshotRevision == Other.SnapshotRevision &&
		RosterRevision == Other.RosterRevision &&
		PayloadHash == Other.PayloadHash;
}

void UWeaponHostMigrationSubsystem::Deinitialize()
{
	StopHostSnapshotTransport();
	ResetPayloadBuffers();
	Super::Deinitialize();
}

bool UWeaponHostMigrationSubsystem::PrepareAuthorityTerm(
	const FGuid& MatchId,
	const FWeaponMigrationRoleMember& ActiveHost,
	const uint64 RosterRevision,
	uint64& OutAuthorityEpoch,
	FString& OutError)
{
	OutAuthorityEpoch = 0;
	OutError.Reset();

	FWeaponMigrationRoleAssignment ProposedAssignment;
	ProposedAssignment.MatchId = MatchId;
	ProposedAssignment.AuthorityEpoch = 1;
	ProposedAssignment.RosterRevision = RosterRevision;
	ProposedAssignment.ActiveHost = ActiveHost;
	if (!ValidateRoleAssignment(ProposedAssignment, OutError))
	{
		return false;
	}

	if (!bHasCurrentRoleAssignment || CurrentRoleAssignment.MatchId != MatchId)
	{
		StopHostSnapshotTransport();
		ResetPayloadBuffers();
		CommonCommittedAck = FWeaponMigrationPayloadAck();
		bHasCommonCommittedPayload = false;
		LastLocalSaveRevision = 0;
		CurrentRoleAssignment = MoveTemp(ProposedAssignment);
		bHasCurrentRoleAssignment = true;
		bCurrentRoleAssignmentIsProvisional = true;
		OutAuthorityEpoch = CurrentRoleAssignment.AuthorityEpoch;
		return true;
	}

	if (!CurrentRoleAssignment.ActiveHost.IdentifiesSameMember(ActiveHost))
	{
		OutError = TEXT("A different Active Host must first begin its deterministic candidate authority term");
		return false;
	}
	if (RosterRevision < CurrentRoleAssignment.RosterRevision)
	{
		OutError = TEXT("Authority role preparation cannot move RosterRevision backwards");
		return false;
	}

	OutAuthorityEpoch = CurrentRoleAssignment.AuthorityEpoch;
	return true;
}

bool UWeaponHostMigrationSubsystem::BeginCandidateAuthorityTerm(
	const FUniqueNetIdRepl& CandidatePlayerId,
	const uint64 ExpectedAuthorityEpoch,
	uint64& OutCandidateEpoch,
	FString& OutError)
{
	OutCandidateEpoch = 0;
	OutError.Reset();
	if (!bHasCurrentRoleAssignment ||
		ExpectedAuthorityEpoch != CurrentRoleAssignment.AuthorityEpoch)
	{
		OutError = TEXT("Candidate term does not match the current AuthorityEpoch");
		return false;
	}

	const EWeaponMigrationRole CandidateRole =
		CurrentRoleAssignment.ResolveRole(CandidatePlayerId);
	const uint64 EpochIncrement = CandidateRole == EWeaponMigrationRole::BackupA
		? 1
		: CandidateRole == EWeaponMigrationRole::BackupB ? 2 : 0;
	if (EpochIncrement == 0 || CurrentRoleAssignment.AuthorityEpoch > MAX_uint64 - EpochIncrement)
	{
		OutError = TEXT("Only the assigned Backup A/B can begin a non-overflowing candidate term");
		return false;
	}

	const FWeaponMigrationRoleMember Candidate = CandidateRole == EWeaponMigrationRole::BackupA
		? CurrentRoleAssignment.BackupA
		: CurrentRoleAssignment.BackupB;
	StopHostSnapshotTransport();
	DiscardPendingPayload();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ChunkAssemblyTimerHandle);
	}
	CurrentRoleAssignment.AuthorityEpoch += EpochIncrement;
	CurrentRoleAssignment.ActiveHost = Candidate;
	CurrentRoleAssignment.BackupA = FWeaponMigrationRoleMember();
	CurrentRoleAssignment.BackupB = FWeaponMigrationRoleMember();
	bCurrentRoleAssignmentIsProvisional = true;
	OutCandidateEpoch = CurrentRoleAssignment.AuthorityEpoch;
	return true;
}

bool UWeaponHostMigrationSubsystem::SetAuthorityRoleAssignment(
	const FWeaponMigrationRoleAssignment& Assignment,
	FString& OutError)
{
	OutError.Reset();
	if (!ValidateRoleAssignment(Assignment, OutError))
	{
		return false;
	}
	if (!bHasCurrentRoleAssignment ||
		Assignment.MatchId != CurrentRoleAssignment.MatchId ||
		Assignment.AuthorityEpoch != CurrentRoleAssignment.AuthorityEpoch ||
		!Assignment.ActiveHost.IdentifiesSameMember(CurrentRoleAssignment.ActiveHost))
	{
		OutError = TEXT("Authority assignment does not match the prepared Match/Epoch/Active Host term");
		return false;
	}
	if (Assignment.RosterRevision < CurrentRoleAssignment.RosterRevision)
	{
		OutError = TEXT("Authority assignment cannot move RosterRevision backwards");
		return false;
	}
	if (!bCurrentRoleAssignmentIsProvisional &&
		Assignment.RosterRevision == CurrentRoleAssignment.RosterRevision &&
		!Assignment.IdentifiesSameAssignment(CurrentRoleAssignment))
	{
		OutError = TEXT("Conflicting migration roles cannot share one RosterRevision");
		return false;
	}

	CurrentRoleAssignment = Assignment;
	bCurrentRoleAssignmentIsProvisional = false;
	return true;
}

bool UWeaponHostMigrationSubsystem::AcceptRoleAssignment(
	const FWeaponMigrationRoleAssignment& Assignment,
	FString& OutError)
{
	OutError.Reset();
	if (!ValidateRoleAssignment(Assignment, OutError))
	{
		return false;
	}
	if (!bHasCurrentRoleAssignment)
	{
		CurrentRoleAssignment = Assignment;
		bHasCurrentRoleAssignment = true;
		bCurrentRoleAssignmentIsProvisional = false;
		return true;
	}
	if (Assignment.MatchId != CurrentRoleAssignment.MatchId)
	{
		OutError = TEXT("Migration role assignment belongs to a different MatchId");
		return false;
	}
	if (Assignment.AuthorityEpoch < CurrentRoleAssignment.AuthorityEpoch)
	{
		OutError = TEXT("Migration role assignment belongs to a stale AuthorityEpoch");
		return false;
	}
	if (Assignment.RosterRevision < CurrentRoleAssignment.RosterRevision)
	{
		OutError = TEXT("Migration role assignment moves RosterRevision backwards");
		return false;
	}

	if (Assignment.AuthorityEpoch == CurrentRoleAssignment.AuthorityEpoch)
	{
		if (!Assignment.ActiveHost.IdentifiesSameMember(CurrentRoleAssignment.ActiveHost) ||
			Assignment.RosterRevision < CurrentRoleAssignment.RosterRevision)
		{
			OutError = TEXT("Same-term migration assignment conflicts with Active Host or RosterRevision");
			return false;
		}
		if (Assignment.RosterRevision == CurrentRoleAssignment.RosterRevision &&
			!Assignment.IdentifiesSameAssignment(CurrentRoleAssignment))
		{
			OutError = TEXT("Same RosterRevision contains conflicting migration roles");
			return false;
		}
	}
	else
	{
		const bool bExpectedBackupA = CurrentRoleAssignment.BackupA.IsAssigned() &&
			CurrentRoleAssignment.AuthorityEpoch < MAX_uint64 &&
			Assignment.AuthorityEpoch == CurrentRoleAssignment.AuthorityEpoch + 1 &&
			Assignment.ActiveHost.IdentifiesSameMember(CurrentRoleAssignment.BackupA);
		const bool bExpectedBackupB = CurrentRoleAssignment.BackupB.IsAssigned() &&
			CurrentRoleAssignment.AuthorityEpoch <= MAX_uint64 - 2 &&
			Assignment.AuthorityEpoch == CurrentRoleAssignment.AuthorityEpoch + 2 &&
			Assignment.ActiveHost.IdentifiesSameMember(CurrentRoleAssignment.BackupB);
		if (!bExpectedBackupA && !bExpectedBackupB)
		{
			OutError = TEXT("New authority term is not the deterministic Backup A/B successor");
			return false;
		}
	}

	StopHostSnapshotTransport();
	DiscardPendingPayload();
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ChunkAssemblyTimerHandle);
	}
	CurrentRoleAssignment = Assignment;
	bCurrentRoleAssignmentIsProvisional = false;
	return true;
}

bool UWeaponHostMigrationSubsystem::AcceptCommonCommittedAnnouncement(
	const FUniqueNetIdRepl& ReceiverPlayerId,
	const FWeaponMigrationPayloadAck& Ack,
	FString& OutError)
{
	OutError.Reset();
	if (!bHasCurrentRoleAssignment ||
		Ack.MatchId != CurrentRoleAssignment.MatchId ||
		Ack.AuthorityEpoch != CurrentRoleAssignment.AuthorityEpoch ||
		Ack.RosterRevision != CurrentRoleAssignment.RosterRevision ||
		Ack.SnapshotRevision <= 0 || Ack.PayloadHash.Num() != WeaponMigrationSHA256Bytes)
	{
		OutError = TEXT("CommonCommitted announcement does not match the current role assignment");
		return false;
	}

	const EWeaponMigrationRole ReceiverRole = GetRoleForPlayer(ReceiverPlayerId);
	if (ReceiverRole == EWeaponMigrationRole::BackupA ||
		ReceiverRole == EWeaponMigrationRole::BackupB)
	{
		FWeaponMigrationPayloadAck LocalCommittedAck;
		if (!GetCommittedAck(LocalCommittedAck) || !LocalCommittedAck.IdentifiesSamePayload(Ack))
		{
			OutError = TEXT("Assigned Backup cannot accept CommonCommitted without identical local bytes");
			return false;
		}
	}

	if (bHasCommonCommittedPayload &&
		Ack.MatchId == CommonCommittedAck.MatchId &&
		Ack.SnapshotRevision < CommonCommittedAck.SnapshotRevision)
	{
		OutError = TEXT("CommonCommitted announcement moves SnapshotRevision backwards");
		return false;
	}
	CommonCommittedAck = Ack;
	bHasCommonCommittedPayload = true;
	return true;
}

EWeaponMigrationRole UWeaponHostMigrationSubsystem::GetRoleForPlayer(
	const FUniqueNetIdRepl& PlayerId) const
{
	return bHasCurrentRoleAssignment
		? CurrentRoleAssignment.ResolveRole(PlayerId)
		: EWeaponMigrationRole::None;
}

bool UWeaponHostMigrationSubsystem::StartHostSnapshotTransport(
	AWeaponPlayerController* BackupA,
	AWeaponPlayerController* BackupB,
	const uint64 SuppliedAuthorityEpoch,
	FString& OutError)
{
	OutError.Reset();
	StopHostSnapshotTransport();

	UWorld* World = BackupA ? BackupA->GetWorld() : nullptr;
	if (!World || World->GetNetMode() == NM_Client || BackupA == BackupB ||
		!BackupB || BackupB->GetWorld() != World ||
		!BackupA->HasAuthority() || !BackupB->HasAuthority() ||
		!BackupA->GetNetConnection() || !BackupB->GetNetConnection())
	{
		OutError = TEXT("Host transport requires two distinct authoritative remote PlayerControllers in one server World");
		return false;
	}
	if (SuppliedAuthorityEpoch == 0)
	{
		OutError = TEXT("Host transport requires a non-zero externally supplied AuthorityEpoch");
		return false;
	}
	const APlayerState* BackupAPlayerState = BackupA->GetPlayerState<APlayerState>();
	const APlayerState* BackupBPlayerState = BackupB->GetPlayerState<APlayerState>();
	if (!bHasCurrentRoleAssignment ||
		SuppliedAuthorityEpoch != CurrentRoleAssignment.AuthorityEpoch ||
		!BackupAPlayerState || !BackupBPlayerState ||
		BackupAPlayerState->GetUniqueId() != CurrentRoleAssignment.BackupA.PlayerId ||
		BackupBPlayerState->GetUniqueId() != CurrentRoleAssignment.BackupB.PlayerId)
	{
		OutError = TEXT("Host transport controllers do not match the current fenced Backup A/B assignment");
		return false;
	}
	if (GetLocalHostSaveSlotName().IsEmpty())
	{
		OutError = TEXT("Host transport requires a non-empty local SaveGame slot name");
		return false;
	}

	HostWorld = World;
	HostBackupA = BackupA;
	HostBackupB = BackupB;
	HostAuthorityEpoch = SuppliedAuthorityEpoch;
	if (!CaptureAndDistributeSnapshot(OutError))
	{
		StopHostSnapshotTransport();
		return false;
	}

	World->GetTimerManager().SetTimer(
		SnapshotCaptureTimerHandle,
		this,
		&UWeaponHostMigrationSubsystem::HandleSnapshotInterval,
		GetSnapshotIntervalSeconds(),
		true);
	return true;
}

void UWeaponHostMigrationSubsystem::StopHostSnapshotTransport()
{
	ClearHostTimers();
	ResetOutgoingTransfer();
	HostBackupA.Reset();
	HostBackupB.Reset();
	HostWorld.Reset();
	HostAuthorityEpoch = 0;
}

bool UWeaponHostMigrationSubsystem::IsHostSnapshotTransportConfigured(
	const AWeaponPlayerController* BackupA,
	const AWeaponPlayerController* BackupB,
	const uint64 AuthorityEpoch) const
{
	return HostWorld.IsValid() && HostBackupA.Get() == BackupA && HostBackupB.Get() == BackupB &&
		HostAuthorityEpoch == AuthorityEpoch;
}

bool UWeaponHostMigrationSubsystem::CaptureAndDistributeSnapshot(FString& OutError)
{
	OutError.Reset();
	if (!IsHostTransportReady(OutError))
	{
		return false;
	}
	if (OutgoingTransfer.bActive)
	{
		OutError = TEXT("A migration payload is already awaiting local save, send completion or A/B ACKs");
		return false;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UWeaponSnapshotCoordinator* Coordinator =
		GameInstance ? GameInstance->GetSubsystem<UWeaponSnapshotCoordinator>() : nullptr;
	if (!Coordinator)
	{
		OutError = TEXT("WeaponSnapshotCoordinator is unavailable during Host capture");
		return false;
	}

	UWeaponMatchSaveGame* SaveGame = nullptr;
	TArray<uint8> PayloadBytes;
	FWeaponMigrationPayloadEnvelope Envelope;
	if (!Coordinator->CaptureSaveGame(HostWorld.Get(), SaveGame, OutError) ||
		!Coordinator->SerializeSaveGame(SaveGame, PayloadBytes, OutError) ||
		!BuildPayloadEnvelope(SaveGame, PayloadBytes, HostAuthorityEpoch, Envelope, OutError))
	{
		return false;
	}

	OutgoingTransfer = FOutgoingPayloadTransfer();
	OutgoingTransfer.Envelope = Envelope;
	OutgoingTransfer.Bytes = MoveTemp(PayloadBytes);
	OutgoingTransfer.bWaitingForLocalSave = true;
	OutgoingTransfer.bActive = true;

	const uint64 RequestSerial = ++LocalSaveRequestSerial;
	FAsyncSaveGameToSlotDelegate SavedDelegate;
	SavedDelegate.BindWeakLambda(
		this,
		[this, RequestSerial](const FString&, const int32, const bool bSuccess)
		{
			HandleLocalSaveComplete(RequestSerial, bSuccess);
		});
	UGameplayStatics::AsyncSaveGameToSlot(
		SaveGame,
		GetLocalHostSaveSlotName(),
		GetLocalHostSaveUserIndex(),
		MoveTemp(SavedDelegate));
	return true;
}

bool UWeaponHostMigrationSubsystem::ReceivePayloadChunk(
	const FUniqueNetIdRepl& ReceiverPlayerId,
	const FWeaponMigrationPayloadEnvelope& Envelope,
	const TArray<uint8>& ChunkBytes,
	bool& bOutAckReady,
	FWeaponMigrationPayloadAck& OutAck,
	FString& OutError)
{
	bOutAckReady = false;
	OutAck = FWeaponMigrationPayloadAck();
	OutError.Reset();
	const EWeaponMigrationRole ReceiverRole = GetRoleForPlayer(ReceiverPlayerId);
	if ((ReceiverRole != EWeaponMigrationRole::BackupA &&
		 ReceiverRole != EWeaponMigrationRole::BackupB) ||
		!ValidateCurrentEnvelope(Envelope, OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Migration chunks are accepted only by the currently assigned Backup A/B");
		}
		return false;
	}

	const bool bHadPending = PendingPayload.bActive;
	const FWeaponMigrationPayloadEnvelope PreviousPendingEnvelope = PendingPayload.Envelope;
	bool bPayloadComplete = false;
	if (!StorePendingChunk(Envelope, ChunkBytes, bPayloadComplete, OutError))
	{
		if (!PendingPayload.bActive)
		{
			if (UWorld* World = GetWorld())
			{
				World->GetTimerManager().ClearTimer(ChunkAssemblyTimerHandle);
			}
		}
		return false;
	}

	if (!bPayloadComplete)
	{
		FWeaponMigrationPayloadAck ExistingAck;
		if (GetCommittedAck(ExistingAck) && ExistingAck.IdentifiesPayload(Envelope))
		{
			OutAck = MoveTemp(ExistingAck);
			bOutAckReady = true;
			return true;
		}

		const bool bStartedNewPending = PendingPayload.bActive &&
			(!bHadPending || !PreviousPendingEnvelope.IdentifiesSamePayload(PendingPayload.Envelope));
		if (bStartedNewPending)
		{
			if (UWorld* World = GetWorld())
			{
				World->GetTimerManager().SetTimer(
					ChunkAssemblyTimerHandle,
					this,
					&UWeaponHostMigrationSubsystem::HandleChunkAssemblyTimeout,
					GetChunkAssemblyTimeoutSeconds(),
					false);
			}
		}
		return true;
	}

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ChunkAssemblyTimerHandle);
	}
	if (!PromotePendingPayload(OutAck, OutError))
	{
		return false;
	}
	bOutAckReady = true;
	return true;
}

bool UWeaponHostMigrationSubsystem::RecordBackupAcknowledgement(
	const AWeaponPlayerController* Sender,
	const FWeaponMigrationPayloadAck& Ack,
	FString& OutError)
{
	OutError.Reset();
	const bool bFromBackupA = Sender && Sender == HostBackupA.Get();
	const bool bFromBackupB = Sender && Sender == HostBackupB.Get();
	if (!bFromBackupA && !bFromBackupB)
	{
		OutError = TEXT("Migration ACK sender is not the configured Backup A or Backup B");
		return false;
	}
	const APlayerState* SenderPlayerState = Sender->GetPlayerState<APlayerState>();
	const EWeaponMigrationRole SenderRole = SenderPlayerState
		? GetRoleForPlayer(SenderPlayerState->GetUniqueId())
		: EWeaponMigrationRole::None;
	if ((bFromBackupA && SenderRole != EWeaponMigrationRole::BackupA) ||
		(bFromBackupB && SenderRole != EWeaponMigrationRole::BackupB))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Migration ACK sender no longer owns its fenced Backup role");
		}
		return false;
	}
	if (bHasCommonCommittedPayload && Ack.IdentifiesSamePayload(CommonCommittedAck))
	{
		return true;
	}
	if (bHasCommonCommittedPayload && Ack.MatchId == CommonCommittedAck.MatchId &&
		Ack.SnapshotRevision < CommonCommittedAck.SnapshotRevision)
	{
		OutError = TEXT("Migration ACK would move CommonCommitted SnapshotRevision backwards");
		return false;
	}
	if (!OutgoingTransfer.bActive || OutgoingTransfer.bWaitingForLocalSave ||
		!Ack.IdentifiesPayload(OutgoingTransfer.Envelope) ||
		!ValidateCurrentEnvelope(OutgoingTransfer.Envelope, OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Migration ACK does not identify the current outgoing payload");
		}
		return false;
	}

	if (bFromBackupA)
	{
		OutgoingTransfer.bBackupAAcked = true;
	}
	if (bFromBackupB)
	{
		OutgoingTransfer.bBackupBAcked = true;
	}
	if (!OutgoingTransfer.bBackupAAcked || !OutgoingTransfer.bBackupBAcked)
	{
		return true;
	}

	CommonCommittedAck = Ack;
	bHasCommonCommittedPayload = true;
	if (UWorld* World = HostWorld.Get())
	{
		World->GetTimerManager().ClearTimer(TransferAckTimerHandle);
	}
	UE_LOG(
		LogLetsDeepRock,
		Log,
		TEXT("Migration revision %lld is CommonCommitted for Match %s Epoch %llu"),
		CommonCommittedAck.SnapshotRevision,
		*CommonCommittedAck.MatchId.ToString(),
		CommonCommittedAck.AuthorityEpoch);
	BroadcastCommonCommittedAck();
	ResetOutgoingTransfer();
	return true;
}

bool UWeaponHostMigrationSubsystem::GetCommonCommittedAck(
	FWeaponMigrationPayloadAck& OutAck) const
{
	OutAck = FWeaponMigrationPayloadAck();
	if (!bHasCommonCommittedPayload)
	{
		return false;
	}
	OutAck = CommonCommittedAck;
	return true;
}

bool UWeaponHostMigrationSubsystem::BuildPayloadEnvelope(
	const UWeaponMatchSaveGame* SaveGame,
	const TArray<uint8>& PayloadBytes,
	const uint64 AuthorityEpoch,
	FWeaponMigrationPayloadEnvelope& OutEnvelope,
	FString& OutError) const
{
	OutEnvelope = FWeaponMigrationPayloadEnvelope();
	OutError.Reset();
	if (!SaveGame || !UWeaponSnapshotCoordinator::ValidateSaveGame(SaveGame, OutError))
	{
		return false;
	}
	if (AuthorityEpoch == 0)
	{
		OutError = TEXT("Migration payload requires a non-zero AuthorityEpoch");
		return false;
	}
	if (PayloadBytes.IsEmpty() || PayloadBytes.Num() > GetMaxSnapshotBytes())
	{
		OutError = TEXT("Serialized migration payload is empty or exceeds MaxSnapshotBytes");
		return false;
	}

	OutEnvelope.MatchId = SaveGame->Snapshot.MatchId;
	OutEnvelope.AuthorityEpoch = AuthorityEpoch;
	OutEnvelope.SnapshotRevision = SaveGame->Snapshot.Revision;
	OutEnvelope.RosterRevision = SaveGame->Snapshot.RosterRevision;
	OutEnvelope.SaveFormatVersion = SaveGame->SaveFormatVersion;
	OutEnvelope.SnapshotSchemaVersion = SaveGame->Snapshot.SchemaVersion;
	OutEnvelope.PayloadByteCount = PayloadBytes.Num();
	OutEnvelope.ChunkCount = FMath::DivideAndRoundUp(PayloadBytes.Num(), GetChunkPayloadBytes());
	if (bHasCurrentRoleAssignment &&
		(OutEnvelope.MatchId != CurrentRoleAssignment.MatchId ||
		 OutEnvelope.AuthorityEpoch != CurrentRoleAssignment.AuthorityEpoch ||
		 OutEnvelope.RosterRevision != CurrentRoleAssignment.RosterRevision))
	{
		OutEnvelope = FWeaponMigrationPayloadEnvelope();
		OutError = TEXT("Captured SaveGame does not match the current fenced migration role assignment");
		return false;
	}
	if (!ComputePayloadHash(PayloadBytes, OutEnvelope.PayloadHash))
	{
		OutEnvelope = FWeaponMigrationPayloadEnvelope();
		OutError = TEXT("Failed to compute migration payload SHA-256");
		return false;
	}
	return true;
}

bool UWeaponHostMigrationSubsystem::StorePendingChunk(
	const FWeaponMigrationPayloadEnvelope& Envelope,
	const TArray<uint8>& ChunkBytes,
	bool& bOutPayloadComplete,
	FString& OutError)
{
	bOutPayloadComplete = false;
	OutError.Reset();
	if (!ValidateEnvelopeShape(Envelope, ChunkBytes.Num(), OutError))
	{
		return false;
	}

	if (CommittedPayload.bValid &&
		Envelope.MatchId == CommittedPayload.Envelope.MatchId &&
		Envelope.AuthorityEpoch == CommittedPayload.Envelope.AuthorityEpoch)
	{
		if (Envelope.SnapshotRevision < CommittedPayload.Envelope.SnapshotRevision ||
			Envelope.RosterRevision < CommittedPayload.Envelope.RosterRevision)
		{
			OutError = TEXT("Migration chunk is older than the Committed payload");
			return false;
		}
		if (Envelope.SnapshotRevision == CommittedPayload.Envelope.SnapshotRevision)
		{
			if (!Envelope.IdentifiesSamePayload(CommittedPayload.Envelope))
			{
				OutError = TEXT("Migration chunk conflicts with the Committed revision");
				return false;
			}
			return true;
		}
	}

	if (PendingPayload.bActive && !Envelope.IdentifiesSamePayload(PendingPayload.Envelope))
	{
		const bool bSameAuthorityTerm = Envelope.MatchId == PendingPayload.Envelope.MatchId &&
			Envelope.AuthorityEpoch == PendingPayload.Envelope.AuthorityEpoch;
		if (!bSameAuthorityTerm ||
			Envelope.SnapshotRevision < PendingPayload.Envelope.SnapshotRevision ||
			Envelope.RosterRevision < PendingPayload.Envelope.RosterRevision)
		{
			OutError = TEXT("Migration chunk belongs to a stale or unexpected authority term");
			return false;
		}
		if (Envelope.SnapshotRevision == PendingPayload.Envelope.SnapshotRevision)
		{
			DiscardPendingPayload();
			OutError = TEXT("Migration chunk conflicts with the current Pending revision");
			return false;
		}
		InitializePendingPayload(Envelope);
	}
	else if (!PendingPayload.bActive)
	{
		InitializePendingPayload(Envelope);
	}

	if (PendingPayload.ReceivedChunks[Envelope.ChunkIndex])
	{
		if (PendingPayload.Chunks[Envelope.ChunkIndex] != ChunkBytes)
		{
			DiscardPendingPayload();
			OutError = TEXT("Conflicting duplicate migration chunk invalidated Pending");
			return false;
		}
		bOutPayloadComplete = IsPendingPayloadComplete();
		return true;
	}

	PendingPayload.Chunks[Envelope.ChunkIndex] = ChunkBytes;
	PendingPayload.ReceivedChunks[Envelope.ChunkIndex] = true;
	++PendingPayload.ReceivedChunkCount;
	PendingPayload.ReceivedByteCount += ChunkBytes.Num();
	bOutPayloadComplete = IsPendingPayloadComplete();
	return true;
}

bool UWeaponHostMigrationSubsystem::PromotePendingPayload(
	FWeaponMigrationPayloadAck& OutAck,
	FString& OutError)
{
	OutAck = FWeaponMigrationPayloadAck();
	OutError.Reset();
	if (!IsPendingPayloadComplete())
	{
		OutError = TEXT("Pending migration payload is incomplete");
		return false;
	}

	TArray<uint8> AssembledBytes;
	AssembledBytes.Reserve(PendingPayload.Envelope.PayloadByteCount);
	for (const TArray<uint8>& Chunk : PendingPayload.Chunks)
	{
		AssembledBytes.Append(Chunk);
	}
	if (AssembledBytes.Num() != PendingPayload.Envelope.PayloadByteCount)
	{
		DiscardPendingPayload();
		OutError = TEXT("Assembled migration payload byte count does not match its envelope");
		return false;
	}

	TArray<uint8> ComputedHash;
	if (!ComputePayloadHash(AssembledBytes, ComputedHash) ||
		!HashesEqual(ComputedHash, PendingPayload.Envelope.PayloadHash))
	{
		DiscardPendingPayload();
		OutError = TEXT("Assembled migration payload SHA-256 does not match its envelope");
		return false;
	}

	UWeaponSnapshotCoordinator* Coordinator =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponSnapshotCoordinator>() : nullptr;
	UWeaponMatchSaveGame* SaveGame = nullptr;
	if (!Coordinator || !Coordinator->DeserializeSaveGame(AssembledBytes, SaveGame, OutError))
	{
		DiscardPendingPayload();
		if (!Coordinator)
		{
			OutError = TEXT("WeaponSnapshotCoordinator is unavailable during payload validation");
		}
		return false;
	}

	const FWeaponMigrationPayloadEnvelope& Envelope = PendingPayload.Envelope;
	if (SaveGame->SaveFormatVersion != Envelope.SaveFormatVersion ||
		SaveGame->Snapshot.SchemaVersion != Envelope.SnapshotSchemaVersion ||
		SaveGame->Snapshot.MatchId != Envelope.MatchId ||
		SaveGame->Snapshot.Revision != Envelope.SnapshotRevision ||
		SaveGame->Snapshot.RosterRevision != Envelope.RosterRevision)
	{
		DiscardPendingPayload();
		OutError = TEXT("Validated SaveGame identity or version does not match its migration envelope");
		return false;
	}

	CommittedPayload.Envelope = Envelope;
	CommittedPayload.Envelope.ChunkIndex = 0;
	CommittedPayload.Bytes = MoveTemp(AssembledBytes);
	CommittedPayload.bValid = true;

	OutAck.MatchId = Envelope.MatchId;
	OutAck.AuthorityEpoch = Envelope.AuthorityEpoch;
	OutAck.SnapshotRevision = Envelope.SnapshotRevision;
	OutAck.RosterRevision = Envelope.RosterRevision;
	OutAck.PayloadHash = Envelope.PayloadHash;
	DiscardPendingPayload();
	return true;
}

void UWeaponHostMigrationSubsystem::DiscardPendingPayload()
{
	PendingPayload = FPendingPayloadBuffer();
}

void UWeaponHostMigrationSubsystem::ResetPayloadBuffers()
{
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(ChunkAssemblyTimerHandle);
	}
	DiscardPendingPayload();
	CommittedPayload = FCommittedPayloadBuffer();
}

bool UWeaponHostMigrationSubsystem::IsPendingPayloadComplete() const
{
	return PendingPayload.bActive &&
		PendingPayload.ReceivedChunkCount == PendingPayload.Envelope.ChunkCount &&
		PendingPayload.ReceivedByteCount == PendingPayload.Envelope.PayloadByteCount;
}

const FWeaponMigrationPayloadEnvelope* UWeaponHostMigrationSubsystem::GetCommittedEnvelope() const
{
	return CommittedPayload.bValid ? &CommittedPayload.Envelope : nullptr;
}

bool UWeaponHostMigrationSubsystem::CopyCommittedPayload(TArray<uint8>& OutBytes) const
{
	OutBytes.Reset();
	if (!CommittedPayload.bValid)
	{
		return false;
	}
	OutBytes = CommittedPayload.Bytes;
	return true;
}

bool UWeaponHostMigrationSubsystem::GetCommittedAck(FWeaponMigrationPayloadAck& OutAck) const
{
	OutAck = FWeaponMigrationPayloadAck();
	if (!CommittedPayload.bValid)
	{
		return false;
	}
	OutAck.MatchId = CommittedPayload.Envelope.MatchId;
	OutAck.AuthorityEpoch = CommittedPayload.Envelope.AuthorityEpoch;
	OutAck.SnapshotRevision = CommittedPayload.Envelope.SnapshotRevision;
	OutAck.RosterRevision = CommittedPayload.Envelope.RosterRevision;
	OutAck.PayloadHash = CommittedPayload.Envelope.PayloadHash;
	return true;
}

bool UWeaponHostMigrationSubsystem::ValidateEnvelopeShape(
	const FWeaponMigrationPayloadEnvelope& Envelope,
	const int32 ChunkByteCount,
	FString& OutError) const
{
	if (Envelope.ProtocolVersion != WeaponMigrationProtocolVersion ||
		Envelope.SaveFormatVersion != WeaponMatchSaveGameVersion ||
		Envelope.SnapshotSchemaVersion != WeaponSnapshotVersion)
	{
		OutError = TEXT("Migration protocol, SaveGame format or Snapshot schema is unsupported");
		return false;
	}
	if (!Envelope.MatchId.IsValid() || Envelope.AuthorityEpoch == 0 ||
		Envelope.SnapshotRevision <= 0 || Envelope.RosterRevision == 0 ||
		Envelope.RosterRevision == MAX_uint64 ||
		Envelope.PayloadHash.Num() != WeaponMigrationSHA256Bytes)
	{
		OutError = TEXT("Migration envelope identity, revision, epoch or hash is invalid");
		return false;
	}
	if (Envelope.PayloadByteCount <= 0 || Envelope.PayloadByteCount > GetMaxSnapshotBytes())
	{
		OutError = TEXT("Migration envelope payload size is invalid");
		return false;
	}

	const int32 ExpectedChunkCount =
		FMath::DivideAndRoundUp(Envelope.PayloadByteCount, GetChunkPayloadBytes());
	if (Envelope.ChunkCount != ExpectedChunkCount ||
		Envelope.ChunkIndex < 0 || Envelope.ChunkIndex >= Envelope.ChunkCount)
	{
		OutError = TEXT("Migration envelope chunk index or count is invalid");
		return false;
	}
	const int32 ChunkOffset = Envelope.ChunkIndex * GetChunkPayloadBytes();
	const int32 ExpectedChunkBytes =
		FMath::Min(GetChunkPayloadBytes(), Envelope.PayloadByteCount - ChunkOffset);
	if (ChunkByteCount != ExpectedChunkBytes)
	{
		OutError = TEXT("Migration chunk size does not match its envelope position");
		return false;
	}
	return true;
}

void UWeaponHostMigrationSubsystem::InitializePendingPayload(
	const FWeaponMigrationPayloadEnvelope& Envelope)
{
	PendingPayload = FPendingPayloadBuffer();
	PendingPayload.Envelope = Envelope;
	PendingPayload.Envelope.ChunkIndex = 0;
	PendingPayload.Chunks.SetNum(Envelope.ChunkCount);
	PendingPayload.ReceivedChunks.Init(false, Envelope.ChunkCount);
	PendingPayload.bActive = true;
}

bool UWeaponHostMigrationSubsystem::ComputePayloadHash(
	const TArray<uint8>& PayloadBytes,
	TArray<uint8>& OutHash)
{
	OutHash.Reset();
	if (PayloadBytes.IsEmpty())
	{
		return false;
	}

	FSHA256Signature Signature;
	if (!FPlatformMisc::GetSHA256Signature(
		PayloadBytes.GetData(),
		static_cast<uint32>(PayloadBytes.Num()),
		Signature))
	{
		return false;
	}
	OutHash.Append(Signature.Signature, WeaponMigrationSHA256Bytes);
	return true;
}

bool UWeaponHostMigrationSubsystem::HashesEqual(
	const TArray<uint8>& A,
	const TArray<uint8>& B)
{
	return A.Num() == WeaponMigrationSHA256Bytes &&
		B.Num() == WeaponMigrationSHA256Bytes &&
		FMemory::Memcmp(A.GetData(), B.GetData(), WeaponMigrationSHA256Bytes) == 0;
}

bool UWeaponHostMigrationSubsystem::ValidateRoleAssignment(
	const FWeaponMigrationRoleAssignment& Assignment,
	FString& OutError)
{
	const auto IsMemberValid = [](const FWeaponMigrationRoleMember& Member, const bool bRequired)
	{
		if (!Member.IsAssigned())
		{
			return !bRequired && !Member.PlayerId.IsValid() && Member.OriginalSlot == INDEX_NONE;
		}
		return Member.OriginalSlot >= 0 && Member.OriginalSlot < 4;
	};

	if (!Assignment.MatchId.IsValid() || Assignment.AuthorityEpoch == 0 ||
		Assignment.RosterRevision == 0 || Assignment.RosterRevision == MAX_uint64 ||
		!IsMemberValid(Assignment.ActiveHost, true) ||
		!IsMemberValid(Assignment.BackupA, false) ||
		!IsMemberValid(Assignment.BackupB, false) ||
		(Assignment.BackupB.IsAssigned() && !Assignment.BackupA.IsAssigned()))
	{
		OutError = TEXT("Migration role assignment identity, revision or member shape is invalid");
		return false;
	}

	TSet<FUniqueNetIdRepl> PlayerIds;
	TSet<int32> Slots;
	const FWeaponMigrationRoleMember* Members[] =
	{
		&Assignment.ActiveHost,
		&Assignment.BackupA,
		&Assignment.BackupB,
	};
	for (const FWeaponMigrationRoleMember* Member : Members)
	{
		if (!Member->IsAssigned())
		{
			continue;
		}
		if (PlayerIds.Contains(Member->PlayerId) || Slots.Contains(Member->OriginalSlot))
		{
			OutError = TEXT("Migration roles contain duplicate PlayerId or OriginalSlot values");
			return false;
		}
		PlayerIds.Add(Member->PlayerId);
		Slots.Add(Member->OriginalSlot);
	}
	if (Assignment.BackupA.IsAssigned() && Assignment.BackupB.IsAssigned() &&
		Assignment.BackupA.OriginalSlot >= Assignment.BackupB.OriginalSlot)
	{
		OutError = TEXT("Backup A/B must be ordered by ascending stable OriginalSlot");
		return false;
	}
	return true;
}

bool UWeaponHostMigrationSubsystem::ValidateCurrentEnvelope(
	const FWeaponMigrationPayloadEnvelope& Envelope,
	FString& OutError) const
{
	if (!bHasCurrentRoleAssignment || bCurrentRoleAssignmentIsProvisional ||
		Envelope.MatchId != CurrentRoleAssignment.MatchId ||
		Envelope.AuthorityEpoch != CurrentRoleAssignment.AuthorityEpoch ||
		Envelope.RosterRevision != CurrentRoleAssignment.RosterRevision)
	{
		OutError = TEXT("Migration payload belongs to a stale or unpublished role assignment");
		return false;
	}
	return true;
}

void UWeaponHostMigrationSubsystem::BroadcastCommonCommittedAck()
{
	UWorld* World = HostWorld.Get();
	if (!World || !bHasCommonCommittedPayload)
	{
		return;
	}
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		if (AWeaponPlayerController* PlayerController = Cast<AWeaponPlayerController>(It->Get()))
		{
			PlayerController->ClientConfirmMigrationCommonCommitted(CommonCommittedAck);
		}
	}
}

bool UWeaponHostMigrationSubsystem::IsHostTransportReady(FString& OutError) const
{
	UWorld* World = HostWorld.Get();
	const AWeaponPlayerController* BackupA = HostBackupA.Get();
	const AWeaponPlayerController* BackupB = HostBackupB.Get();
	if (!World || World->GetNetMode() == NM_Client || HostAuthorityEpoch == 0 ||
		!bHasCurrentRoleAssignment || bCurrentRoleAssignmentIsProvisional ||
		HostAuthorityEpoch != CurrentRoleAssignment.AuthorityEpoch ||
		!BackupA || !BackupB || BackupA == BackupB ||
		BackupA->GetWorld() != World || BackupB->GetWorld() != World ||
		!BackupA->HasAuthority() || !BackupB->HasAuthority() ||
		!BackupA->GetNetConnection() || !BackupB->GetNetConnection())
	{
		OutError = TEXT("Configured Host backup transport is no longer valid");
		return false;
	}
	const APlayerState* BackupAPlayerState = BackupA->GetPlayerState<APlayerState>();
	const APlayerState* BackupBPlayerState = BackupB->GetPlayerState<APlayerState>();
	if (!BackupAPlayerState || !BackupBPlayerState ||
		BackupAPlayerState->GetUniqueId() != CurrentRoleAssignment.BackupA.PlayerId ||
		BackupBPlayerState->GetUniqueId() != CurrentRoleAssignment.BackupB.PlayerId)
	{
		OutError = TEXT("Configured Host controllers no longer own the fenced Backup A/B roles");
		return false;
	}
	return true;
}

void UWeaponHostMigrationSubsystem::HandleSnapshotInterval()
{
	if (OutgoingTransfer.bActive)
	{
		return;
	}

	FString Error;
	if (!CaptureAndDistributeSnapshot(Error))
	{
		UE_LOG(LogLetsDeepRock, Warning, TEXT("Periodic migration Snapshot capture failed: %s"), *Error);
	}
}

void UWeaponHostMigrationSubsystem::HandleLocalSaveComplete(
	const uint64 RequestSerial,
	const bool bSuccess)
{
	if (RequestSerial != LocalSaveRequestSerial || !OutgoingTransfer.bActive ||
		!OutgoingTransfer.bWaitingForLocalSave)
	{
		return;
	}
	if (!bSuccess)
	{
		UE_LOG(
			LogLetsDeepRock,
			Error,
			TEXT("Local Host SaveGame write failed for migration revision %lld"),
			OutgoingTransfer.Envelope.SnapshotRevision);
		ResetOutgoingTransfer();
		return;
	}

	LastLocalSaveRevision = OutgoingTransfer.Envelope.SnapshotRevision;
	OutgoingTransfer.bWaitingForLocalSave = false;
	SendNextOutgoingChunk();
}

void UWeaponHostMigrationSubsystem::SendNextOutgoingChunk()
{
	if (!OutgoingTransfer.bActive || OutgoingTransfer.bWaitingForLocalSave)
	{
		return;
	}

	FString Error;
	if (!IsHostTransportReady(Error))
	{
		UE_LOG(LogLetsDeepRock, Warning, TEXT("Migration payload send aborted: %s"), *Error);
		ResetOutgoingTransfer();
		return;
	}

	if (OutgoingTransfer.NextChunkIndex >= OutgoingTransfer.Envelope.ChunkCount)
	{
		if (UWorld* World = HostWorld.Get())
		{
			World->GetTimerManager().SetTimer(
				TransferAckTimerHandle,
				this,
				&UWeaponHostMigrationSubsystem::HandleTransferAckTimeout,
				GetTransferAckTimeoutSeconds(),
				false);
		}
		return;
	}

	FWeaponMigrationPayloadEnvelope ChunkEnvelope = OutgoingTransfer.Envelope;
	ChunkEnvelope.ChunkIndex = OutgoingTransfer.NextChunkIndex;
	const int32 ChunkOffset = ChunkEnvelope.ChunkIndex * GetChunkPayloadBytes();
	const int32 ChunkByteCount = FMath::Min(
		GetChunkPayloadBytes(),
		OutgoingTransfer.Bytes.Num() - ChunkOffset);
	TArray<uint8> ChunkBytes;
	ChunkBytes.Append(OutgoingTransfer.Bytes.GetData() + ChunkOffset, ChunkByteCount);

	HostBackupA->ClientReceiveMigrationPayloadChunk(ChunkEnvelope, ChunkBytes);
	HostBackupB->ClientReceiveMigrationPayloadChunk(ChunkEnvelope, ChunkBytes);
	++OutgoingTransfer.NextChunkIndex;

	if (UWorld* World = HostWorld.Get())
	{
		ChunkSendTimerHandle = World->GetTimerManager().SetTimerForNextTick(
			this,
			&UWeaponHostMigrationSubsystem::SendNextOutgoingChunk);
	}
}

void UWeaponHostMigrationSubsystem::HandleTransferAckTimeout()
{
	if (!OutgoingTransfer.bActive)
	{
		return;
	}
	UE_LOG(
		LogLetsDeepRock,
		Warning,
		TEXT("Migration revision %lld ACK timeout (BackupA=%s BackupB=%s); CommonCommitted remains %lld"),
		OutgoingTransfer.Envelope.SnapshotRevision,
		OutgoingTransfer.bBackupAAcked ? TEXT("true") : TEXT("false"),
		OutgoingTransfer.bBackupBAcked ? TEXT("true") : TEXT("false"),
		GetCommonCommittedRevision());
	ResetOutgoingTransfer();
}

void UWeaponHostMigrationSubsystem::HandleChunkAssemblyTimeout()
{
	if (!PendingPayload.bActive)
	{
		return;
	}
	UE_LOG(
		LogLetsDeepRock,
		Warning,
		TEXT("Discarding incomplete migration revision %lld after chunk assembly timeout"),
		PendingPayload.Envelope.SnapshotRevision);
	DiscardPendingPayload();
}

void UWeaponHostMigrationSubsystem::ResetOutgoingTransfer()
{
	++LocalSaveRequestSerial;
	if (UWorld* World = HostWorld.Get())
	{
		World->GetTimerManager().ClearTimer(ChunkSendTimerHandle);
		World->GetTimerManager().ClearTimer(TransferAckTimerHandle);
	}
	OutgoingTransfer = FOutgoingPayloadTransfer();
}

void UWeaponHostMigrationSubsystem::ClearHostTimers()
{
	if (UWorld* World = HostWorld.Get())
	{
		World->GetTimerManager().ClearTimer(SnapshotCaptureTimerHandle);
		World->GetTimerManager().ClearTimer(ChunkSendTimerHandle);
		World->GetTimerManager().ClearTimer(TransferAckTimerHandle);
	}
}
