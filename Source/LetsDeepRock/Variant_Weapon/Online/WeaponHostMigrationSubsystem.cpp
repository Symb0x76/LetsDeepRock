// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponHostMigrationSubsystem.h"
#include "LetsDeepRock.h"
#include "WeaponMatchSaveGame.h"
#include "WeaponSnapshot.h"
#include "WeaponSnapshotCoordinator.h"
#include "WeaponSessionSubsystem.h"
#include "WeaponPlayerController.h"
#include "WeaponGameMode.h"
#include "WeaponGameState.h"
#include "Containers/Ticker.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GameFramework/PlayerState.h"
#include "IPlatformCrypto.h"
#include "Kismet/GameplayStatics.h"
#include "TimerManager.h"
#include "UObject/UObjectGlobals.h"

namespace WeaponMigration
{
	constexpr float RecoveryTickIntervalSeconds = 0.25f;
	constexpr double RecoverySearchRetrySeconds = 1.0;
}

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

bool FWeaponRecoverySessionDescriptor::IsValid(const bool bRequireAttemptId) const
{
	return MatchId.IsValid() && AuthorityEpoch > 0 &&
		BaseSnapshotRevision > 0 && BaseRosterRevision > 0 &&
		(!bRequireAttemptId || RecoveryAttemptId.IsValid()) &&
		(bRequireAttemptId || !RecoveryAttemptId.IsValid()) &&
		Candidate.IsAssigned() && Candidate.OriginalSlot >= 0 && Candidate.OriginalSlot < 4 &&
		!MapPackageName.IsEmpty() && MaxPlayers >= 1 && MaxPlayers <= 4 &&
		PayloadHash.Num() == WeaponMigrationSHA256Bytes &&
		(State == EWeaponRecoverySessionState::Restoring ||
		 State == EWeaponRecoverySessionState::Ready);
}

bool FWeaponRecoverySessionDescriptor::IdentifiesSameBaseline(
	const FWeaponRecoverySessionDescriptor& Other) const
{
	return MatchId == Other.MatchId &&
		AuthorityEpoch == Other.AuthorityEpoch &&
		BaseSnapshotRevision == Other.BaseSnapshotRevision &&
		BaseRosterRevision == Other.BaseRosterRevision &&
		Candidate.IdentifiesSameMember(Other.Candidate) &&
		MapPackageName == Other.MapPackageName &&
		MaxPlayers == Other.MaxPlayers &&
		PayloadHash == Other.PayloadHash;
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

const TCHAR* UWeaponHostMigrationSubsystem::LexMigrationState(
	const EWeaponHostMigrationState State)
{
	switch (State)
	{
	case EWeaponHostMigrationState::Idle: return TEXT("Idle");
	case EWeaponHostMigrationState::Monitoring: return TEXT("Monitoring");
	case EWeaponHostMigrationState::WaitingForDisconnectTravel: return TEXT("WaitingForDisconnectTravel");
	case EWeaponHostMigrationState::PreparingCandidate: return TEXT("PreparingCandidate");
	case EWeaponHostMigrationState::SearchingCandidate: return TEXT("SearchingCandidate");
	case EWeaponHostMigrationState::JoiningCandidate: return TEXT("JoiningCandidate");
	case EWeaponHostMigrationState::Restoring: return TEXT("Restoring");
	case EWeaponHostMigrationState::AdvertisingCandidate: return TEXT("AdvertisingCandidate");
	case EWeaponHostMigrationState::WaitingForReadyAcks: return TEXT("WaitingForReadyAcks");
	case EWeaponHostMigrationState::PlannedHandoff: return TEXT("PlannedHandoff");
	case EWeaponHostMigrationState::LoadingLocalFork: return TEXT("LoadingLocalFork");
	case EWeaponHostMigrationState::PreparingLocalFork: return TEXT("PreparingLocalFork");
	case EWeaponHostMigrationState::RestoringLocalFork: return TEXT("RestoringLocalFork");
	case EWeaponHostMigrationState::AdvertisingLocalFork: return TEXT("AdvertisingLocalFork");
	case EWeaponHostMigrationState::Failed: return TEXT("Failed");
	default: return TEXT("Unknown");
	}
}

void UWeaponHostMigrationSubsystem::SetMigrationState(
	const EWeaponHostMigrationState NewState,
	const FString& Message,
	const float Progress,
	const bool bShowOverlay)
{
	const EWeaponHostMigrationState PreviousState = MigrationState;
	MigrationState = NewState;
	bMigrationOverlayVisible = bShowOverlay;
	MigrationOverlayMessage = FText::FromString(Message);
	MigrationProgress = FMath::Clamp(Progress, 0.0f, 1.0f);

	const FGuid MatchId = ActiveRecoverySession.MatchId.IsValid()
		? ActiveRecoverySession.MatchId
		: RecoveryBaselineAck.MatchId.IsValid()
			? RecoveryBaselineAck.MatchId
			: bHasCurrentRoleAssignment ? CurrentRoleAssignment.MatchId : FGuid();
	const uint64 Epoch = ActiveRecoverySession.AuthorityEpoch > 0
		? ActiveRecoverySession.AuthorityEpoch
		: RecoveryBaselineAck.AuthorityEpoch > 0
			? RecoveryBaselineAck.AuthorityEpoch
			: bHasCurrentRoleAssignment ? CurrentRoleAssignment.AuthorityEpoch : 0;
	const int64 SnapshotRevision = RecoveryBaselineAck.SnapshotRevision;
	const uint64 RosterRevision = RecoveryReadyRosterRevision > 0
		? RecoveryReadyRosterRevision
		: RecoveryBaselineAck.RosterRevision;
	UE_LOG(
		LogLetsDeepRock,
		Log,
		TEXT("HostMigration state %s -> %s Match=%s Epoch=%llu Snapshot=%lld Roster=%llu Attempt=%s Candidate=%s: %s"),
		LexMigrationState(PreviousState),
		LexMigrationState(NewState),
		*MatchId.ToString(),
		Epoch,
		SnapshotRevision,
		RosterRevision,
		*ActiveRecoverySession.RecoveryAttemptId.ToString(),
		*ActiveRecoverySession.Candidate.PlayerId.ToDebugString(),
		*Message);
	OnMigrationPresentationChanged.Broadcast(
		bMigrationOverlayVisible,
		MigrationOverlayMessage,
		MigrationProgress);
}

void UWeaponHostMigrationSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UWeaponSessionSubsystem>();

	if (GEngine)
	{
		NetworkFailureDelegateHandle = GEngine->OnNetworkFailure().AddUObject(
			this,
			&UWeaponHostMigrationSubsystem::HandleNetworkFailure);
	}
	PostLoadMapDelegateHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
		this,
		&UWeaponHostMigrationSubsystem::HandlePostLoadMap);

	if (UWeaponSessionSubsystem* SessionSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>())
	{
		RecoveryWorldPreparedDelegateHandle = SessionSubsystem->OnRecoveryWorldPrepared.AddUObject(
			this,
			&UWeaponHostMigrationSubsystem::HandleRecoveryWorldPrepared);
		RecoveryHostCompleteDelegateHandle = SessionSubsystem->OnRecoveryHostComplete.AddUObject(
			this,
			&UWeaponHostMigrationSubsystem::HandleRecoveryHostComplete);
		RecoveryJoinCompleteDelegateHandle = SessionSubsystem->OnRecoveryJoinComplete.AddUObject(
			this,
			&UWeaponHostMigrationSubsystem::HandleRecoveryJoinComplete);
		LanHostCompleteDelegateHandle = SessionSubsystem->OnLanHostComplete.AddUObject(
			this,
			&UWeaponHostMigrationSubsystem::HandleLanHostComplete);
	}

	RecoveryTickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UWeaponHostMigrationSubsystem::TickRecovery),
		WeaponMigration::RecoveryTickIntervalSeconds);
}

void UWeaponHostMigrationSubsystem::Deinitialize()
{
	if (RecoveryTickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(RecoveryTickerHandle);
		RecoveryTickerHandle.Reset();
	}
	if (GEngine && NetworkFailureDelegateHandle.IsValid())
	{
		GEngine->OnNetworkFailure().Remove(NetworkFailureDelegateHandle);
		NetworkFailureDelegateHandle.Reset();
	}
	if (PostLoadMapDelegateHandle.IsValid())
	{
		FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapDelegateHandle);
		PostLoadMapDelegateHandle.Reset();
	}
	if (UWeaponSessionSubsystem* SessionSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>())
	{
		SessionSubsystem->OnRecoveryWorldPrepared.Remove(RecoveryWorldPreparedDelegateHandle);
		SessionSubsystem->OnRecoveryHostComplete.Remove(RecoveryHostCompleteDelegateHandle);
		SessionSubsystem->OnRecoveryJoinComplete.Remove(RecoveryJoinCompleteDelegateHandle);
		SessionSubsystem->OnLanHostComplete.Remove(LanHostCompleteDelegateHandle);
	}
	++LocalForkRequestSerial;
	PendingLocalForkSaveGame = nullptr;
	ResetRecoveryAttemptContext();
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
		CommonCommittedRoleAssignment = FWeaponMigrationRoleAssignment();
		bHasCommonCommittedPayload = false;
		bHasCommonCommittedRoleAssignment = false;
		bBackupAConfirmedCommonCommitted = false;
		bBackupBConfirmedCommonCommitted = false;
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
	if (MigrationState == EWeaponHostMigrationState::Idle)
	{
		SetMigrationState(
			EWeaponHostMigrationState::Monitoring,
			TEXT("Host migration redundancy is monitoring the current authority term."),
			1.0f,
			false);
	}
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
		if (MigrationState == EWeaponHostMigrationState::Idle)
		{
			SetMigrationState(
				EWeaponHostMigrationState::Monitoring,
				TEXT("Host migration redundancy is monitoring the current authority term."),
				1.0f,
				false);
		}
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
	if (MigrationState == EWeaponHostMigrationState::Idle)
	{
		SetMigrationState(
			EWeaponHostMigrationState::Monitoring,
			TEXT("Host migration redundancy is monitoring the current authority term."),
			1.0f,
			false);
	}
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
	if (bHasCommonCommittedPayload &&
		Ack.MatchId == CommonCommittedAck.MatchId &&
		Ack.SnapshotRevision < CommonCommittedAck.SnapshotRevision)
	{
		OutError = TEXT("CommonCommitted announcement moves SnapshotRevision backwards");
		return false;
	}

	const EWeaponMigrationRole ReceiverRole = GetRoleForPlayer(ReceiverPlayerId);
	if (ReceiverRole == EWeaponMigrationRole::BackupA ||
		ReceiverRole == EWeaponMigrationRole::BackupB)
	{
		FWeaponMigrationPayloadAck ExistingCommonAck;
		if (GetLocalCommonCommittedAck(ExistingCommonAck) &&
			ExistingCommonAck.IdentifiesSamePayload(Ack))
		{
			CommonCommittedAck = Ack;
			bHasCommonCommittedPayload = true;
			CommonCommittedRoleAssignment = CurrentRoleAssignment;
			bHasCommonCommittedRoleAssignment = true;
			return true;
		}
		FWeaponMigrationPayloadAck LocalCommittedAck;
		if (!GetCommittedAck(LocalCommittedAck) || !LocalCommittedAck.IdentifiesSamePayload(Ack))
		{
			OutError = TEXT("Assigned Backup cannot accept CommonCommitted without identical local bytes");
			return false;
		}
		CommonCommittedPayload = CommittedPayload;
	}

	CommonCommittedAck = Ack;
	bHasCommonCommittedPayload = true;
	CommonCommittedRoleAssignment = CurrentRoleAssignment;
	bHasCommonCommittedRoleAssignment = true;
	bBackupAConfirmedCommonCommitted = false;
	bBackupBConfirmedCommonCommitted = false;
	return true;
}

EWeaponMigrationRole UWeaponHostMigrationSubsystem::GetRoleForPlayer(
	const FUniqueNetIdRepl& PlayerId) const
{
	return bHasCurrentRoleAssignment
		? CurrentRoleAssignment.ResolveRole(PlayerId)
		: EWeaponMigrationRole::None;
}

void UWeaponHostMigrationSubsystem::HandleNetworkFailure(
	UWorld* World,
	UNetDriver* NetDriver,
	const ENetworkFailure::Type FailureType,
	const FString& ErrorString)
{
	const bool bHostConnectionFailure = FailureType == ENetworkFailure::ConnectionLost ||
		FailureType == ENetworkFailure::ConnectionTimeout;
	const bool bRecoveringCandidateConnection =
		MigrationState == EWeaponHostMigrationState::JoiningCandidate ||
		MigrationState == EWeaponHostMigrationState::WaitingForReadyAcks;
	const bool bCandidateConnectionFailure =
		bRecoveringCandidateConnection &&
		(FailureType == ENetworkFailure::PendingConnectionFailure || bHostConnectionFailure);
	if (!World || World->GetGameInstance() != GetGameInstance() || !NetDriver ||
		NetDriver->GetNetMode() != NM_Client ||
		(!bHostConnectionFailure && !bCandidateConnectionFailure))
	{
		return;
	}

	if (bRecoveringCandidateConnection)
	{
		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("Recovery candidate connection failed before PostLoadMap: %s"),
			*ErrorString);
		bRecoverySearchInFlight = false;
		if (RecoveryCandidateRole == EWeaponMigrationRole::BackupA)
		{
			if (RecoveryOriginAssignment.MatchId.IsValid())
			{
				CurrentRoleAssignment = RecoveryOriginAssignment;
				bHasCurrentRoleAssignment = true;
				bCurrentRoleAssignmentIsProvisional = false;
			}
			RecoveryCandidateRole = EWeaponMigrationRole::BackupB;
			RecoveryCandidateDeadlineSeconds = FPlatformTime::Seconds() + GetCandidateRecoveryTimeoutSeconds();
			bRecoveryTravelCompleted = false;
			SetMigrationState(
				EWeaponHostMigrationState::WaitingForDisconnectTravel,
				TEXT("Backup A disconnected; preparing the clean Backup B attempt."),
				0.35f,
				true);
		}
		else
		{
			SetMigrationState(
				EWeaponHostMigrationState::Failed,
				TEXT("Both migration candidates are unavailable. Returning to the lobby."),
				0.0f,
				true);
		}
		return;
	}

	if (MigrationState != EWeaponHostMigrationState::Monitoring)
	{
		return;
	}

	FString Error;
	if (!PrimeRecoveryFromCommittedBaseline(World, Error))
	{
		UE_LOG(
			LogLetsDeepRock,
			Error,
			TEXT("Host migration cannot start after network failure: %s (engine error: %s)"),
			*Error,
			*ErrorString);
		SetMigrationState(
			EWeaponHostMigrationState::Failed,
			Error,
			0.0f,
			true);
	}
}

bool UWeaponHostMigrationSubsystem::PrimeRecoveryFromCommittedBaseline(
	UWorld* SourceWorld,
	FString& OutError)
{
	OutError.Reset();
	UGameInstance* GameInstance = GetGameInstance();
	const APlayerController* LocalController = GameInstance
		? GameInstance->GetFirstLocalPlayerController(SourceWorld)
		: nullptr;
	const APlayerState* LocalPlayerState = LocalController
		? LocalController->GetPlayerState<APlayerState>()
		: nullptr;
	const AWeaponGameState* GameState = SourceWorld
		? SourceWorld->GetGameState<AWeaponGameState>()
		: nullptr;
	FWeaponMigrationPayloadAck CommonAck;
	if (!SourceWorld || !bHasCommonCommittedRoleAssignment ||
		!GetCommonCommittedAck(CommonAck) || !LocalPlayerState ||
		!LocalPlayerState->GetUniqueId().IsValid() || !GameState ||
		!CommonCommittedRoleAssignment.BackupA.IsAssigned() ||
		CommonAck.MatchId != CommonCommittedRoleAssignment.MatchId ||
		CommonAck.AuthorityEpoch != CommonCommittedRoleAssignment.AuthorityEpoch ||
		CommonAck.RosterRevision != CommonCommittedRoleAssignment.RosterRevision)
	{
		OutError = TEXT("A published fixed-role assignment and CommonCommitted baseline are required");
		return false;
	}

	const FUniqueNetIdRepl LocalPlayerId = LocalPlayerState->GetUniqueId();
	const EWeaponMigrationRole LocalRole = CommonCommittedRoleAssignment.ResolveRole(LocalPlayerId);
	if (LocalRole == EWeaponMigrationRole::BackupA || LocalRole == EWeaponMigrationRole::BackupB)
	{
		FWeaponMigrationPayloadAck LocalCommonAck;
		if (!GetLocalCommonCommittedAck(LocalCommonAck) ||
			!LocalCommonAck.IdentifiesSamePayload(CommonAck))
		{
			OutError = TEXT("Assigned candidate no longer has the exact CommonCommitted bytes");
			return false;
		}
	}

	const FString MapPackageName = UWorld::RemovePIEPrefix(SourceWorld->GetOutermost()->GetName());
	if (MapPackageName.IsEmpty() || GameState->GetMaxPlayers() < 1 || GameState->GetMaxPlayers() > 4)
	{
		OutError = TEXT("Failed World map or capacity is invalid for recovery discovery");
		return false;
	}

	StopHostSnapshotTransport();
	DiscardPendingPayload();
	RecoveryOriginAssignment = CommonCommittedRoleAssignment;
	RecoveryBaselineAck = CommonAck;
	RecoveryLocalPlayerId = LocalPlayerId;
	CurrentRoleAssignment = CommonCommittedRoleAssignment;
	bHasCurrentRoleAssignment = true;
	bCurrentRoleAssignmentIsProvisional = false;
	ActiveRecoverySession = FWeaponRecoverySessionDescriptor();
	ActiveRecoverySession.MapPackageName = MapPackageName;
	ActiveRecoverySession.MaxPlayers = GameState->GetMaxPlayers();
	RecoveryCandidateRole = EWeaponMigrationRole::BackupA;
	RecoveryCandidateDeadlineSeconds = FPlatformTime::Seconds() + GetCandidateRecoveryTimeoutSeconds();
	NextRecoverySearchSeconds = 0.0;
	bRecoverySearchInFlight = false;
	bRecoveryTravelCompleted = false;
	SetMigrationState(
		EWeaponHostMigrationState::WaitingForDisconnectTravel,
		TEXT("Authority was lost. Waiting for the disposable lobby World before trying Backup A."),
		0.1f,
		true);

	UE_LOG(
		LogLetsDeepRock,
		Warning,
		TEXT("Authority loss fenced for Match %s Epoch %llu CommonRevision %lld; waiting for disconnect travel"),
		*CommonAck.MatchId.ToString(),
		CommonAck.AuthorityEpoch,
		CommonAck.SnapshotRevision);
	return true;
}

void UWeaponHostMigrationSubsystem::HandlePostLoadMap(UWorld* LoadedWorld)
{
	if (!LoadedWorld || LoadedWorld->GetGameInstance() != GetGameInstance())
	{
		return;
	}

	if (MigrationState == EWeaponHostMigrationState::WaitingForDisconnectTravel)
	{
		bRecoveryTravelCompleted = true;
		return;
	}

	if (MigrationState == EWeaponHostMigrationState::JoiningCandidate &&
		LoadedWorld->GetNetMode() == NM_Client)
	{
		bRecoveryTravelCompleted = true;
		SetMigrationState(
			EWeaponHostMigrationState::JoiningCandidate,
			TEXT("Connected to the successor. Waiting for its fenced RecoveryReady request."),
			0.85f,
			true);
	}
}

bool UWeaponHostMigrationSubsystem::BuildRecoverySessionDescriptor(
	const EWeaponMigrationRole CandidateRole,
	const bool bCreateAttempt,
	FWeaponRecoverySessionDescriptor& OutDescriptor,
	FString& OutError) const
{
	OutDescriptor = FWeaponRecoverySessionDescriptor();
	OutError.Reset();
	const uint64 EpochIncrement = CandidateRole == EWeaponMigrationRole::BackupA
		? 1
		: CandidateRole == EWeaponMigrationRole::BackupB ? 2 : 0;
	const FWeaponMigrationRoleMember Candidate = CandidateRole == EWeaponMigrationRole::BackupA
		? RecoveryOriginAssignment.BackupA
		: CandidateRole == EWeaponMigrationRole::BackupB
			? RecoveryOriginAssignment.BackupB
			: FWeaponMigrationRoleMember();
	if (EpochIncrement == 0 || !Candidate.IsAssigned() ||
		RecoveryOriginAssignment.AuthorityEpoch > MAX_uint64 - EpochIncrement ||
		!RecoveryBaselineAck.IdentifiesSamePayload(CommonCommittedAck) ||
		ActiveRecoverySession.MapPackageName.IsEmpty() ||
		ActiveRecoverySession.MaxPlayers < 1 || ActiveRecoverySession.MaxPlayers > 4)
	{
		OutError = TEXT("Recovery candidate, baseline, map or authority increment is invalid");
		return false;
	}

	OutDescriptor.MatchId = RecoveryOriginAssignment.MatchId;
	OutDescriptor.AuthorityEpoch = RecoveryOriginAssignment.AuthorityEpoch + EpochIncrement;
	OutDescriptor.BaseSnapshotRevision = RecoveryBaselineAck.SnapshotRevision;
	OutDescriptor.BaseRosterRevision = RecoveryBaselineAck.RosterRevision;
	OutDescriptor.RecoveryAttemptId = bCreateAttempt ? FGuid::NewGuid() : FGuid();
	OutDescriptor.Candidate = Candidate;
	OutDescriptor.MapPackageName = ActiveRecoverySession.MapPackageName;
	OutDescriptor.MaxPlayers = ActiveRecoverySession.MaxPlayers;
	OutDescriptor.PayloadHash = RecoveryBaselineAck.PayloadHash;
	OutDescriptor.State = bCreateAttempt
		? EWeaponRecoverySessionState::Restoring
		: EWeaponRecoverySessionState::Ready;
	if (!OutDescriptor.IsValid(bCreateAttempt))
	{
		OutDescriptor = FWeaponRecoverySessionDescriptor();
		OutError = TEXT("Constructed Recovery Session descriptor is invalid");
		return false;
	}
	return true;
}

bool UWeaponHostMigrationSubsystem::EnterRecoveryCandidatePhase(
	const EWeaponMigrationRole CandidateRole,
	FString& OutError)
{
	OutError.Reset();
	FWeaponRecoverySessionDescriptor Descriptor;
	const bool bLocalCandidate = CandidateRole == EWeaponMigrationRole::BackupA
		? RecoveryOriginAssignment.BackupA.PlayerId == RecoveryLocalPlayerId
		: CandidateRole == EWeaponMigrationRole::BackupB &&
			RecoveryOriginAssignment.BackupB.PlayerId == RecoveryLocalPlayerId;
	if (!BuildRecoverySessionDescriptor(CandidateRole, bLocalCandidate, Descriptor, OutError))
	{
		return false;
	}

	RecoveryCandidateRole = CandidateRole;
	if (CandidateRole != EWeaponMigrationRole::BackupA || RecoveryCandidateDeadlineSeconds <= 0.0)
	{
		RecoveryCandidateDeadlineSeconds = FPlatformTime::Seconds() + GetCandidateRecoveryTimeoutSeconds();
	}
	NextRecoverySearchSeconds = 0.0;
	bRecoverySearchInFlight = false;
	bRecoveryTravelCompleted = false;
	bRecoveryReadyRequestReceived = false;
	RecoveryPublicationRosterRevision = 0;
	RecoveryReadyRosterRevision = 0;
	RecoveryReadyPlayerAcks.Reset();
	ActiveRecoverySession = Descriptor;

	if (!bLocalCandidate)
	{
		SetMigrationState(
			EWeaponHostMigrationState::SearchingCandidate,
			CandidateRole == EWeaponMigrationRole::BackupA
				? TEXT("Searching for Backup A's fenced Recovery Session.")
				: TEXT("Searching for Backup B's fenced Recovery Session."),
			0.35f,
			true);
		return true;
	}

	FWeaponMigrationPayloadAck LocalCommonAck;
	uint64 CandidateEpoch = 0;
	if (!GetLocalCommonCommittedAck(LocalCommonAck) ||
		!LocalCommonAck.IdentifiesSamePayload(RecoveryBaselineAck) ||
		!BeginCandidateAuthorityTerm(
			RecoveryLocalPlayerId,
			RecoveryOriginAssignment.AuthorityEpoch,
			CandidateEpoch,
			OutError) ||
		CandidateEpoch != Descriptor.AuthorityEpoch)
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Local candidate could not prepare the deterministic authority term");
		}
		return false;
	}

	UWeaponSessionSubsystem* SessionSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	SetMigrationState(
		EWeaponHostMigrationState::PreparingCandidate,
		TEXT("Preparing a clean gated listen World for Snapshot restore."),
		0.3f,
		true);
	if (!SessionSubsystem || !SessionSubsystem->PrepareRecoveryHost(Descriptor))
	{
		OutError = TEXT("Recovery Session subsystem could not prepare the candidate listen World");
		return false;
	}
	return true;
}

bool UWeaponHostMigrationSubsystem::TickRecovery(const float DeltaSeconds)
{
	(void)DeltaSeconds;
	const double Now = FPlatformTime::Seconds();
	if (MigrationState == EWeaponHostMigrationState::PlannedHandoff &&
		PlannedHandoffDeadlineSeconds > 0.0 && Now >= PlannedHandoffDeadlineSeconds)
	{
		CompletePlannedHostHandoff(
			false,
			TEXT("Final A/B CommonCommitted convergence deadline expired."));
		return true;
	}

	if (MigrationState == EWeaponHostMigrationState::WaitingForReadyAcks ||
		MigrationState == EWeaponHostMigrationState::JoiningCandidate)
	{
		UWorld* World = GetWorld();
		if (World && World->GetNetMode() == NM_ListenServer &&
			MigrationState == EWeaponHostMigrationState::WaitingForReadyAcks)
		{
			FString Error;
			if (!RefreshRecoveryReadyFence(Error) || !TryReleaseRecoveryBarrier(Error))
			{
				RestartRecoveryCandidateAttempt(Error);
			}
		}
		else if (World && World->GetNetMode() == NM_Client && bRecoveryTravelCompleted)
		{
			// 客户端完成连接的条件是"新主机恢复已完成"(屏障已释放),无论是否收到过
			// RecoveryReady 请求:迟到的玩家可能在新主机已进入 Monitoring 后才加入,
			// 此时不再有请求发出,直接进游戏。屏障未释放时继续等待请求。
			const AWeaponGameState* GameState = World->GetGameState<AWeaponGameState>();
			if (GameState && !GameState->IsGameplayRecoveryBlocked())
			{
				CompleteRecoveryConnection();
			}
		}
		return true;
	}

	if (MigrationState == EWeaponHostMigrationState::WaitingForDisconnectTravel &&
		bRecoveryTravelCompleted)
	{
		UWeaponSessionSubsystem* SessionSubsystem =
			GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
		if (SessionSubsystem &&
			SessionSubsystem->GetCurrentOperation() == EWeaponSessionOperation::Idle)
		{
			bRecoveryTravelCompleted = false;
			FString Error;
			if (!EnterRecoveryCandidatePhase(RecoveryCandidateRole, Error))
			{
				if (RecoveryCandidateRole == EWeaponMigrationRole::BackupA)
				{
					AdvanceRecoveryCandidate(Error);
				}
				else
				{
					FailRecoveryToLobby(Error);
				}
			}
		}
	}
	const bool bCandidateTimedState =
		MigrationState == EWeaponHostMigrationState::WaitingForDisconnectTravel ||
		MigrationState == EWeaponHostMigrationState::PreparingCandidate ||
		MigrationState == EWeaponHostMigrationState::SearchingCandidate ||
		MigrationState == EWeaponHostMigrationState::JoiningCandidate ||
		MigrationState == EWeaponHostMigrationState::Restoring ||
		MigrationState == EWeaponHostMigrationState::AdvertisingCandidate;
	if (bCandidateTimedState && RecoveryCandidateDeadlineSeconds > 0.0 &&
		Now >= RecoveryCandidateDeadlineSeconds)
	{
		AdvanceRecoveryCandidate(TEXT("Candidate recovery deadline expired"));
		return true;
	}

	if (MigrationState != EWeaponHostMigrationState::SearchingCandidate ||
		bRecoverySearchInFlight || Now < NextRecoverySearchSeconds)
	{
		return true;
	}

	UWeaponSessionSubsystem* SessionSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	if (!SessionSubsystem ||
		SessionSubsystem->GetCurrentOperation() != EWeaponSessionOperation::Idle)
	{
		return true;
	}

	bRecoverySearchInFlight = true;
	NextRecoverySearchSeconds = Now + WeaponMigration::RecoverySearchRetrySeconds;
	if (!SessionSubsystem->FindAndJoinRecoverySession(ActiveRecoverySession))
	{
		bRecoverySearchInFlight = false;
	}
	return true;
}

void UWeaponHostMigrationSubsystem::HandleRecoveryWorldPrepared(
	const bool bSuccess,
	const FString& Message)
{
	if (MigrationState == EWeaponHostMigrationState::PreparingLocalFork)
	{
		if (!bSuccess)
		{
			FailLocalSaveFork(Message);
			return;
		}

		SetMigrationState(
			EWeaponHostMigrationState::RestoringLocalFork,
			TEXT("Restoring the local SaveGame under a new MatchId."),
			0.55f,
			true);
		FString Error;
		if (!RestoreLocalSaveFork(Error))
		{
			FailLocalSaveFork(Error);
		}
		return;
	}

	if (MigrationState != EWeaponHostMigrationState::PreparingCandidate)
	{
		return;
	}
	if (!bSuccess)
	{
		AdvanceRecoveryCandidate(Message);
		return;
	}

	SetMigrationState(
		EWeaponHostMigrationState::Restoring,
		TEXT("Validating, staging and applying the committed Snapshot."),
		0.55f,
		true);
	FString Error;
	if (!RestoreCandidateWorld(Error))
	{
		AdvanceRecoveryCandidate(Error);
		return;
	}

	const AWeaponGameMode* GameMode = GetWorld()
		? GetWorld()->GetAuthGameMode<AWeaponGameMode>()
		: nullptr;
	if (!GameMode)
	{
		AdvanceRecoveryCandidate(TEXT("Restored candidate has no authoritative GameMode"));
		return;
	}
	RecoveryPublicationRosterRevision = GameMode->GetRosterRevision();
	ActiveRecoverySession.State = EWeaponRecoverySessionState::Ready;
	SetMigrationState(
		EWeaponHostMigrationState::AdvertisingCandidate,
		TEXT("Publishing the fenced RecoveryReady Session while gameplay remains blocked."),
		0.72f,
		true);
	UWeaponSessionSubsystem* SessionSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	if (!SessionSubsystem || !SessionSubsystem->AdvertiseRecoverySession(ActiveRecoverySession))
	{
		AdvanceRecoveryCandidate(TEXT("Restored candidate could not advertise its Recovery Session"));
	}
}

bool UWeaponHostMigrationSubsystem::RestoreCandidateWorld(FString& OutError)
{
	OutError.Reset();
	UWorld* World = GetWorld();
	AWeaponGameMode* GameMode = World ? World->GetAuthGameMode<AWeaponGameMode>() : nullptr;
	AWeaponGameState* GameState = World ? World->GetGameState<AWeaponGameState>() : nullptr;
	UWeaponSnapshotCoordinator* Coordinator =
		GetGameInstance()->GetSubsystem<UWeaponSnapshotCoordinator>();
	TArray<uint8> PayloadBytes;
	FWeaponMigrationPayloadAck LocalCommonAck;
	UWeaponMatchSaveGame* SaveGame = nullptr;
	if (!World || World->GetNetMode() != NM_ListenServer || !GameMode || !GameState ||
		!GameState->IsGameplayRecoveryBlocked() || !Coordinator ||
		!GetLocalCommonCommittedAck(LocalCommonAck) ||
		!LocalCommonAck.IdentifiesSamePayload(RecoveryBaselineAck) ||
		!CopyCommonCommittedPayload(PayloadBytes) ||
		!Coordinator->DeserializeSaveGame(PayloadBytes, SaveGame, OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("Candidate World, barrier or CommonCommitted payload is unavailable");
		}
		return false;
	}

	if (!SaveGame || SaveGame->Snapshot.MatchId != RecoveryBaselineAck.MatchId ||
		SaveGame->Snapshot.Revision != RecoveryBaselineAck.SnapshotRevision ||
		SaveGame->Snapshot.RosterRevision != RecoveryBaselineAck.RosterRevision ||
		SaveGame->MapPackageName != ActiveRecoverySession.MapPackageName ||
		SaveGame->MaxPlayers != ActiveRecoverySession.MaxPlayers ||
		!bHasCurrentRoleAssignment || !bCurrentRoleAssignmentIsProvisional ||
		CurrentRoleAssignment.MatchId != ActiveRecoverySession.MatchId ||
		CurrentRoleAssignment.AuthorityEpoch != ActiveRecoverySession.AuthorityEpoch ||
		!CurrentRoleAssignment.ActiveHost.IdentifiesSameMember(ActiveRecoverySession.Candidate))
	{
		OutError = TEXT("Candidate SaveGame or prepared authority term does not match the Recovery Session");
		return false;
	}

	if (!Coordinator->RestoreSaveGame(SaveGame, OutError))
	{
		return false;
	}
	if (!GameState->IsGameplayRecoveryBlocked() ||
		Coordinator->GetOrCreateActiveMatchId() != RecoveryBaselineAck.MatchId)
	{
		OutError = TEXT("Restore did not preserve the gated Match identity");
		return false;
	}
	return true;
}

void UWeaponHostMigrationSubsystem::HandleRecoveryHostComplete(
	const bool bSuccess,
	const FString& Message)
{
	if (MigrationState != EWeaponHostMigrationState::AdvertisingCandidate)
	{
		return;
	}
	if (!bSuccess)
	{
		AdvanceRecoveryCandidate(Message);
		return;
	}

	AWeaponGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AWeaponGameMode>() : nullptr;
	if (!GameMode || !GetWorld() || GetWorld()->GetNetMode() != NM_ListenServer ||
		!ActiveRecoverySession.RecoveryAttemptId.IsValid() ||
		GameMode->GetRosterRevision() != RecoveryPublicationRosterRevision)
	{
		RestartRecoveryCandidateAttempt(
			TEXT("Recovery roster changed before RecoveryReady publication completed"));
		return;
	}

	UE_LOG(
		LogLetsDeepRock,
		Log,
		TEXT("RecoveryReady Match %s Epoch %llu Attempt %s Revision %lld Candidate %s"),
		*ActiveRecoverySession.MatchId.ToString(),
		ActiveRecoverySession.AuthorityEpoch,
		*ActiveRecoverySession.RecoveryAttemptId.ToString(),
		ActiveRecoverySession.BaseSnapshotRevision,
		*ActiveRecoverySession.Candidate.PlayerId.ToDebugString());

	RecoveryCandidateDeadlineSeconds = 0.0;
	RecoveryReadyRosterRevision = 0;
	RecoveryReadyPlayerAcks.Reset();
	SetMigrationState(
		EWeaponHostMigrationState::WaitingForReadyAcks,
		TEXT("RecoveryReady is published. Waiting for roster survivors and presentation ACKs."),
		0.82f,
		true);
	FString Error;
	if (!RefreshRecoveryReadyFence(Error) || !TryReleaseRecoveryBarrier(Error))
	{
		RestartRecoveryCandidateAttempt(Error);
	}
}

void UWeaponHostMigrationSubsystem::HandleRecoveryJoinComplete(
	const bool bSuccess,
	const FString& Message)
{
	if (MigrationState != EWeaponHostMigrationState::SearchingCandidate &&
		MigrationState != EWeaponHostMigrationState::JoiningCandidate)
	{
		return;
	}
	bRecoverySearchInFlight = false;
	if (!bSuccess)
	{
		NextRecoverySearchSeconds = FPlatformTime::Seconds() +
			WeaponMigration::RecoverySearchRetrySeconds;
		return;
	}

	const UWeaponSessionSubsystem* SessionSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	const FWeaponRecoverySessionDescriptor* Selected = SessionSubsystem
		? SessionSubsystem->GetSelectedRecoverySession()
		: nullptr;
	if (!Selected || !Selected->IdentifiesSameBaseline(ActiveRecoverySession) ||
		!Selected->IsValid(true))
	{
		AdvanceRecoveryCandidate(TEXT("Joined Recovery Session identity changed after search"));
		return;
	}

	ActiveRecoverySession = *Selected;
	SetMigrationState(
		EWeaponHostMigrationState::JoiningCandidate,
		TEXT("Joining the exact RecoveryReady Session selected by the fenced search."),
		0.75f,
		true);
	UE_LOG(
		LogLetsDeepRock,
		Log,
		TEXT("Joining Recovery Session Match %s Epoch %llu Attempt %s: %s"),
		*Selected->MatchId.ToString(),
		Selected->AuthorityEpoch,
		*Selected->RecoveryAttemptId.ToString(),
		*Message);
}

bool UWeaponHostMigrationSubsystem::RefreshRecoveryReadyFence(FString& OutError)
{
	OutError.Reset();
	UWorld* World = GetWorld();
	AWeaponGameMode* GameMode = World ? World->GetAuthGameMode<AWeaponGameMode>() : nullptr;
	AWeaponGameState* GameState = World ? World->GetGameState<AWeaponGameState>() : nullptr;
	if (MigrationState != EWeaponHostMigrationState::WaitingForReadyAcks ||
		!World || World->GetNetMode() != NM_ListenServer || !GameMode || !GameState ||
		!GameState->IsGameplayRecoveryBlocked() || !ActiveRecoverySession.IsValid(true))
	{
		OutError = TEXT("RecoveryReady ACK fence requires the active gated candidate World");
		return false;
	}

	const uint64 CurrentRosterRevision = GameMode->GetRosterRevision();
	if (CurrentRosterRevision == 0 || CurrentRosterRevision == MAX_uint64)
	{
		OutError = TEXT("RecoveryReady ACK fence has an invalid RosterRevision");
		return false;
	}
	if (CurrentRosterRevision == RecoveryReadyRosterRevision)
	{
		return true;
	}

	RecoveryReadyRosterRevision = CurrentRosterRevision;
	RecoveryReadyPlayerAcks.Reset();
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		AWeaponPlayerController* Controller = Cast<AWeaponPlayerController>(It->Get());
		const APlayerState* PlayerState = Controller
			? Controller->GetPlayerState<APlayerState>()
			: nullptr;
		const FUniqueNetIdRepl PlayerId = PlayerState
			? PlayerState->GetUniqueId()
			: FUniqueNetIdRepl();
		const FWeaponRecoveryRosterEntry* RosterEntry = GameMode->GetRecoveryRoster().Find(PlayerId);
		if (!Controller || !PlayerId.IsValid() || !RosterEntry ||
			RosterEntry->Status != EWeaponRecoveryPlayerStatus::Ready)
		{
			continue;
		}

		if (Controller->IsLocalController())
		{
			RecoveryReadyPlayerAcks.Add(PlayerId);
			continue;
		}
		Controller->ClientRequestRecoveryReadyAcknowledgement(
			ActiveRecoverySession.MatchId,
			ActiveRecoverySession.AuthorityEpoch,
			RecoveryReadyRosterRevision,
			ActiveRecoverySession.RecoveryAttemptId);
	}

	UE_LOG(
		LogLetsDeepRock,
		Log,
		TEXT("RecoveryReady fence issued Match=%s Epoch=%llu Snapshot=%lld Roster=%llu Attempt=%s Candidate=%s"),
		*ActiveRecoverySession.MatchId.ToString(),
		ActiveRecoverySession.AuthorityEpoch,
		ActiveRecoverySession.BaseSnapshotRevision,
		RecoveryReadyRosterRevision,
		*ActiveRecoverySession.RecoveryAttemptId.ToString(),
		*ActiveRecoverySession.Candidate.PlayerId.ToDebugString());
	return true;
}

bool UWeaponHostMigrationSubsystem::TryReleaseRecoveryBarrier(FString& OutError)
{
	OutError.Reset();
	UWorld* World = GetWorld();
	AWeaponGameMode* GameMode = World ? World->GetAuthGameMode<AWeaponGameMode>() : nullptr;
	if (MigrationState != EWeaponHostMigrationState::WaitingForReadyAcks ||
		!GameMode || RecoveryReadyRosterRevision == 0 ||
		GameMode->GetRosterRevision() != RecoveryReadyRosterRevision)
	{
		OutError = TEXT("RecoveryReady release check does not match the current roster fence");
		return false;
	}

	for (const auto& [PlayerId, RosterEntry] : GameMode->GetRecoveryRoster())
	{
		if (RosterEntry.Status == EWeaponRecoveryPlayerStatus::Pending)
		{
			return true;
		}
		if (RosterEntry.Status == EWeaponRecoveryPlayerStatus::Ready &&
			!RecoveryReadyPlayerAcks.Contains(PlayerId))
		{
			return true;
		}
	}

	if (!GameMode->ReleaseGameplayRecoveryBarrier())
	{
		OutError = TEXT("All RecoveryReady ACKs arrived but the gameplay barrier could not be released");
		return false;
	}
	UE_LOG(
		LogLetsDeepRock,
		Log,
		TEXT("RecoveryReady complete Match=%s Epoch=%llu Snapshot=%lld Roster=%llu Attempt=%s Candidate=%s"),
		*ActiveRecoverySession.MatchId.ToString(),
		ActiveRecoverySession.AuthorityEpoch,
		ActiveRecoverySession.BaseSnapshotRevision,
		RecoveryReadyRosterRevision,
		*ActiveRecoverySession.RecoveryAttemptId.ToString(),
		*ActiveRecoverySession.Candidate.PlayerId.ToDebugString());
	CompleteRecoveryConnection();
	return true;
}

bool UWeaponHostMigrationSubsystem::AcceptRecoveryReadyRequest(
	const FGuid& MatchId,
	const uint64 AuthorityEpoch,
	const uint64 RosterRevision,
	const FGuid& RecoveryAttemptId,
	FString& OutError)
{
	OutError.Reset();
	if ((MigrationState != EWeaponHostMigrationState::JoiningCandidate &&
		 MigrationState != EWeaponHostMigrationState::WaitingForReadyAcks) ||
		!bRecoveryTravelCompleted || !ActiveRecoverySession.IsValid(true) ||
		MatchId != ActiveRecoverySession.MatchId ||
		AuthorityEpoch != ActiveRecoverySession.AuthorityEpoch ||
		RecoveryAttemptId != ActiveRecoverySession.RecoveryAttemptId ||
		RosterRevision < ActiveRecoverySession.BaseRosterRevision ||
		RosterRevision < RecoveryReadyRosterRevision || RosterRevision == MAX_uint64)
	{
		OutError = TEXT("RecoveryReady request is stale or does not identify the joined recovery attempt");
		return false;
	}

	RecoveryReadyRosterRevision = RosterRevision;
	bRecoveryReadyRequestReceived = true;
	SetMigrationState(
		EWeaponHostMigrationState::WaitingForReadyAcks,
		TEXT("Successor state arrived. Acknowledging local presentation readiness."),
		0.9f,
		true);
	return true;
}

bool UWeaponHostMigrationSubsystem::RecordRecoveryReadyAcknowledgement(
	const AWeaponPlayerController* Sender,
	const FGuid& MatchId,
	const uint64 AuthorityEpoch,
	const uint64 RosterRevision,
	const FGuid& RecoveryAttemptId,
	FString& OutError)
{
	OutError.Reset();
	UWorld* World = GetWorld();
	AWeaponGameMode* GameMode = World ? World->GetAuthGameMode<AWeaponGameMode>() : nullptr;
	const APlayerState* PlayerState = Sender ? Sender->GetPlayerState<APlayerState>() : nullptr;
	const FUniqueNetIdRepl PlayerId = PlayerState
		? PlayerState->GetUniqueId()
		: FUniqueNetIdRepl();
	const FWeaponRecoveryRosterEntry* RosterEntry = GameMode
		? GameMode->GetRecoveryRoster().Find(PlayerId)
		: nullptr;
	if (MigrationState != EWeaponHostMigrationState::WaitingForReadyAcks ||
		!World || World->GetNetMode() != NM_ListenServer || !Sender || Sender->GetWorld() != World ||
		!GameMode || !PlayerId.IsValid() || !RosterEntry ||
		RosterEntry->Status != EWeaponRecoveryPlayerStatus::Ready ||
		MatchId != ActiveRecoverySession.MatchId ||
		AuthorityEpoch != ActiveRecoverySession.AuthorityEpoch ||
		RosterRevision != RecoveryReadyRosterRevision ||
		RecoveryAttemptId != ActiveRecoverySession.RecoveryAttemptId)
	{
		OutError = TEXT("RecoveryReady ACK is stale, unfenced or not from a Ready connected survivor");
		return false;
	}

	RecoveryReadyPlayerAcks.Add(PlayerId);
	UE_LOG(
		LogLetsDeepRock,
		Log,
		TEXT("RecoveryReady ACK Match=%s Epoch=%llu Snapshot=%lld Roster=%llu Attempt=%s Player=%s"),
		*MatchId.ToString(),
		AuthorityEpoch,
		ActiveRecoverySession.BaseSnapshotRevision,
		RosterRevision,
		*RecoveryAttemptId.ToString(),
		*PlayerId.ToDebugString());
	return TryReleaseRecoveryBarrier(OutError);
}

void UWeaponHostMigrationSubsystem::RestartRecoveryCandidateAttempt(const FString& Reason)
{
	UE_LOG(
		LogLetsDeepRock,
		Warning,
		TEXT("Restarting recovery attempt Match=%s Epoch=%llu Snapshot=%lld Roster=%llu Attempt=%s Candidate=%s: %s"),
		*RecoveryBaselineAck.MatchId.ToString(),
		ActiveRecoverySession.AuthorityEpoch,
		RecoveryBaselineAck.SnapshotRevision,
		RecoveryReadyRosterRevision,
		*ActiveRecoverySession.RecoveryAttemptId.ToString(),
		*ActiveRecoverySession.Candidate.PlayerId.ToDebugString(),
		*Reason);

	StopHostSnapshotTransport();
	if (AWeaponGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AWeaponGameMode>() : nullptr)
	{
		GameMode->EnterGameplayRecoveryBarrier();
	}
	if (bCurrentRoleAssignmentIsProvisional && RecoveryOriginAssignment.MatchId.IsValid())
	{
		CurrentRoleAssignment = RecoveryOriginAssignment;
		bCurrentRoleAssignmentIsProvisional = false;
	}

	RecoveryCandidateDeadlineSeconds = FPlatformTime::Seconds() + GetCandidateRecoveryTimeoutSeconds();
	RecoveryReadyRosterRevision = 0;
	RecoveryReadyPlayerAcks.Reset();
	bRecoveryReadyRequestReceived = false;
	bRecoverySearchInFlight = false;
	bRecoveryTravelCompleted = false;
	SetMigrationState(
		EWeaponHostMigrationState::WaitingForDisconnectTravel,
		TEXT("Roster changed before publication; restarting the same candidate in a clean World."),
		0.2f,
		true);
	UWeaponSessionSubsystem* SessionSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	if (!SessionSubsystem ||
		!SessionSubsystem->ReturnToRecoveryLobby(FName(*ActiveRecoverySession.MapPackageName)))
	{
		AdvanceRecoveryCandidate(TEXT("Unable to discard the stale recovery attempt World"));
	}
}

void UWeaponHostMigrationSubsystem::AdvanceRecoveryCandidate(const FString& Reason)
{
	UE_LOG(
		LogLetsDeepRock,
		Warning,
		TEXT("Recovery candidate %s failed for Match %s Epoch %llu Revision %lld Roster %llu Attempt %s CandidateId %s: %s"),
		RecoveryCandidateRole == EWeaponMigrationRole::BackupA ? TEXT("A") : TEXT("B"),
		*RecoveryBaselineAck.MatchId.ToString(),
		ActiveRecoverySession.AuthorityEpoch,
		RecoveryBaselineAck.SnapshotRevision,
		RecoveryBaselineAck.RosterRevision,
		*ActiveRecoverySession.RecoveryAttemptId.ToString(),
		*ActiveRecoverySession.Candidate.PlayerId.ToDebugString(),
		*Reason);

	if (RecoveryCandidateRole != EWeaponMigrationRole::BackupA)
	{
		FailRecoveryToLobby(Reason);
		return;
	}

	StopHostSnapshotTransport();
	if (AWeaponGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AWeaponGameMode>() : nullptr)
	{
		GameMode->EnterGameplayRecoveryBarrier();
	}
	if (bCurrentRoleAssignmentIsProvisional && RecoveryOriginAssignment.MatchId.IsValid())
	{
		CurrentRoleAssignment = RecoveryOriginAssignment;
		bCurrentRoleAssignmentIsProvisional = false;
	}

	RecoveryCandidateRole = EWeaponMigrationRole::BackupB;
	RecoveryCandidateDeadlineSeconds = FPlatformTime::Seconds() + GetCandidateRecoveryTimeoutSeconds();
	bRecoverySearchInFlight = false;
	bRecoveryTravelCompleted = false;
	SetMigrationState(
		EWeaponHostMigrationState::WaitingForDisconnectTravel,
		TEXT("Backup A failed. Discarding its World before the identical-byte Backup B attempt."),
		0.2f,
		true);
	UWeaponSessionSubsystem* SessionSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	if (!SessionSubsystem ||
		!SessionSubsystem->ReturnToRecoveryLobby(FName(*ActiveRecoverySession.MapPackageName)))
	{
		FailRecoveryToLobby(TEXT("Unable to return to a clean World before Backup B"));
	}
}

void UWeaponHostMigrationSubsystem::FailRecoveryToLobby(const FString& Reason)
{
	UE_LOG(
		LogLetsDeepRock,
		Error,
		TEXT("Host migration exhausted Backup A/B for Match %s Epoch %llu Revision %lld Roster %llu Attempt %s: %s"),
		*RecoveryBaselineAck.MatchId.ToString(),
		ActiveRecoverySession.AuthorityEpoch,
		RecoveryBaselineAck.SnapshotRevision,
		RecoveryBaselineAck.RosterRevision,
		*ActiveRecoverySession.RecoveryAttemptId.ToString(),
		*Reason);
	StopHostSnapshotTransport();
	if (AWeaponGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AWeaponGameMode>() : nullptr)
	{
		GameMode->EnterGameplayRecoveryBarrier();
	}
	SetMigrationState(
		EWeaponHostMigrationState::Failed,
		TEXT("Backup A and Backup B are exhausted. Waiting in the lobby."),
		0.0f,
		true);
	if (UWeaponSessionSubsystem* SessionSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>())
	{
		SessionSubsystem->ReturnToRecoveryLobby(FName(*ActiveRecoverySession.MapPackageName));
	}
	ResetRecoveryAttemptContext();
}

void UWeaponHostMigrationSubsystem::CompleteRecoveryConnection()
{
	SetMigrationState(
		EWeaponHostMigrationState::Monitoring,
		TEXT("Host migration completed. Gameplay is authoritative and live."),
		1.0f,
		false);
	ResetRecoveryAttemptContext();
}

void UWeaponHostMigrationSubsystem::ResetRecoveryAttemptContext()
{
	RecoveryCandidateRole = EWeaponMigrationRole::None;
	RecoveryOriginAssignment = FWeaponMigrationRoleAssignment();
	RecoveryBaselineAck = FWeaponMigrationPayloadAck();
	ActiveRecoverySession = FWeaponRecoverySessionDescriptor();
	RecoveryLocalPlayerId = FUniqueNetIdRepl();
	RecoveryCandidateDeadlineSeconds = 0.0;
	NextRecoverySearchSeconds = 0.0;
	bRecoverySearchInFlight = false;
	bRecoveryTravelCompleted = false;
	bRecoveryReadyRequestReceived = false;
	RecoveryPublicationRosterRevision = 0;
	RecoveryReadyRosterRevision = 0;
	RecoveryReadyPlayerAcks.Reset();
}

bool UWeaponHostMigrationSubsystem::RequestPlannedHostHandoff()
{
	UWorld* World = GetWorld();
	AWeaponGameMode* GameMode = World ? World->GetAuthGameMode<AWeaponGameMode>() : nullptr;
	FWeaponMigrationPayloadAck PreviousCommonAck;
	FString Error;
	if (MigrationState != EWeaponHostMigrationState::Monitoring ||
		!World || World->GetNetMode() != NM_ListenServer || !GameMode ||
		!GetCommonCommittedAck(PreviousCommonAck) ||
		!bHasCurrentRoleAssignment || !bHasCommonCommittedRoleAssignment ||
		!CommonCommittedRoleAssignment.IdentifiesSameAssignment(CurrentRoleAssignment) ||
		!bBackupAConfirmedCommonCommitted || !bBackupBConfirmedCommonCommitted ||
		!IsHostTransportReady(Error))
	{
		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("Planned Host handoff rejected: %s"),
			Error.IsEmpty()
				? TEXT("a live listen Host with two backups and a converged current-role CommonCommitted baseline is required")
				: *Error);
		return false;
	}

	if (OutgoingTransfer.bActive)
	{
		ResetOutgoingTransfer();
	}
	if (!CaptureAndDistributeSnapshot(Error))
	{
		UE_LOG(LogLetsDeepRock, Warning, TEXT("Planned Host final capture failed: %s"), *Error);
		return false;
	}
	PlannedHandoffTargetRevision = OutgoingTransfer.Envelope.SnapshotRevision;
	if (!GameMode->EnterGameplayRecoveryBarrier())
	{
		ResetOutgoingTransfer();
		PlannedHandoffTargetRevision = 0;
		UE_LOG(LogLetsDeepRock, Error, TEXT("Planned Host handoff could not close the gameplay barrier"));
		return false;
	}

	World->GetTimerManager().ClearTimer(SnapshotCaptureTimerHandle);
	PlannedHandoffDeadlineSeconds =
		FPlatformTime::Seconds() + GetTransferAckTimeoutSeconds();
	SetMigrationState(
		EWeaponHostMigrationState::PlannedHandoff,
		TEXT("Gameplay is frozen while the final Snapshot is committed to Backup A and Backup B."),
		0.25f,
		true);
	UE_LOG(
		LogLetsDeepRock,
		Log,
		TEXT("Planned handoff final revision %lld started; previous CommonCommitted revision is %lld"),
		PlannedHandoffTargetRevision,
		PreviousCommonAck.SnapshotRevision);
	return true;
}

void UWeaponHostMigrationSubsystem::CompletePlannedHostHandoff(
	const bool bFinalRevisionCommitted,
	const FString& Reason)
{
	if (MigrationState != EWeaponHostMigrationState::PlannedHandoff)
	{
		return;
	}
	if (!bFinalRevisionCommitted)
	{
		CancelPlannedHostHandoff(Reason);
		return;
	}

	FWeaponMigrationPayloadAck SelectedCommonAck;
	if (!GetCommonCommittedAck(SelectedCommonAck) ||
		SelectedCommonAck.SnapshotRevision != PlannedHandoffTargetRevision ||
		!bBackupAConfirmedCommonCommitted || !bBackupBConfirmedCommonCommitted)
	{
		CancelPlannedHostHandoff(TEXT("No recoverable CommonCommitted baseline is available"));
		return;
	}

	UWorld* World = GetWorld();
	FString Error;
	if (!PrimeRecoveryFromCommittedBaseline(World, Error))
	{
		CancelPlannedHostHandoff(Error);
		return;
	}
	PlannedHandoffTargetRevision = 0;
	PlannedHandoffDeadlineSeconds = 0.0;
	UE_LOG(
		LogLetsDeepRock,
		Log,
		TEXT("Planned Host departure selected CommonCommitted revision %lld: %s"),
		SelectedCommonAck.SnapshotRevision,
		*Reason);

	UWeaponSessionSubsystem* SessionSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	if (!SessionSubsystem ||
		!SessionSubsystem->ReturnToRecoveryLobby(FName(*ActiveRecoverySession.MapPackageName)))
	{
		CancelPlannedHostHandoff(TEXT("The Host could not leave for a standalone recovery lobby"));
	}
}

void UWeaponHostMigrationSubsystem::CancelPlannedHostHandoff(const FString& Reason)
{
	PlannedHandoffTargetRevision = 0;
	PlannedHandoffDeadlineSeconds = 0.0;
	StopHostSnapshotTransport();
	ResetRecoveryAttemptContext();

	AWeaponGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AWeaponGameMode>() : nullptr;
	if (GameMode && GameMode->ReleaseGameplayRecoveryBarrier())
	{
		SetMigrationState(
			EWeaponHostMigrationState::Monitoring,
			FString::Printf(TEXT("Planned handoff was cancelled: %s"), *Reason),
			1.0f,
			false);
		return;
	}
	SetMigrationState(
		EWeaponHostMigrationState::Failed,
		FString::Printf(TEXT("Planned handoff failed closed: %s"), *Reason),
		0.0f,
		true);
}

bool UWeaponHostMigrationSubsystem::RestartLatestLocalSaveAsNewMatch(
	const FString& ServerName)
{
	UWorld* World = GetWorld();
	const UWeaponSessionSubsystem* SessionSubsystem =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>() : nullptr;
	if ((MigrationState != EWeaponHostMigrationState::Idle &&
		 MigrationState != EWeaponHostMigrationState::Failed) ||
		!World || World->GetNetMode() != NM_Standalone || !SessionSubsystem ||
		SessionSubsystem->GetCurrentOperation() != EWeaponSessionOperation::Idle ||
		GetLocalHostSaveSlotName().IsEmpty())
	{
		return false;
	}

	PendingLocalForkServerName = ServerName.TrimStartAndEnd();
	if (PendingLocalForkServerName.IsEmpty())
	{
		PendingLocalForkServerName = TEXT("LetsDeepRock Local Save Fork");
	}
	const uint64 RequestSerial = ++LocalForkRequestSerial;
	SetMigrationState(
		EWeaponHostMigrationState::LoadingLocalFork,
		TEXT("Loading the latest local Host SaveGame for an explicit new-Match fork."),
		0.15f,
		true);

	FAsyncLoadGameFromSlotDelegate LoadedDelegate;
	LoadedDelegate.BindWeakLambda(
		this,
		[this, RequestSerial](const FString&, const int32, USaveGame* LoadedSaveGame)
		{
			HandleLocalForkSaveLoaded(RequestSerial, LoadedSaveGame);
		});
	UGameplayStatics::AsyncLoadGameFromSlot(
		GetLocalHostSaveSlotName(),
		GetLocalHostSaveUserIndex(),
		MoveTemp(LoadedDelegate));
	return true;
}

void UWeaponHostMigrationSubsystem::HandleLocalForkSaveLoaded(
	const uint64 RequestSerial,
	USaveGame* LoadedSaveGame)
{
	if (RequestSerial != LocalForkRequestSerial ||
		MigrationState != EWeaponHostMigrationState::LoadingLocalFork)
	{
		return;
	}

	UWeaponMatchSaveGame* SaveGame = Cast<UWeaponMatchSaveGame>(LoadedSaveGame);
	UWeaponSnapshotCoordinator* Coordinator =
		GetGameInstance()->GetSubsystem<UWeaponSnapshotCoordinator>();
	UWorld* World = GetWorld();
	const APlayerController* LocalController = GetGameInstance()
		? GetGameInstance()->GetFirstLocalPlayerController(World)
		: nullptr;
	const APlayerState* LocalPlayerState = LocalController
		? LocalController->GetPlayerState<APlayerState>()
		: nullptr;
	const FUniqueNetIdRepl LocalPlayerId = LocalPlayerState
		? LocalPlayerState->GetUniqueId()
		: FUniqueNetIdRepl();
	FString Error;
	if (!Coordinator || !SaveGame || !LocalPlayerId.IsValid() ||
		!UWeaponSnapshotCoordinator::ValidateSaveGame(SaveGame, Error))
	{
		FailLocalSaveFork(
			Error.IsEmpty()
				? TEXT("The local SaveGame or current stable player identity is unavailable")
				: Error);
		return;
	}

	const FWeaponRecoveryRosterSnapshot* LocalRosterEntry = SaveGame->Snapshot.RecoveryRoster.FindByPredicate(
		[&LocalPlayerId](const FWeaponRecoveryRosterSnapshot& Entry)
		{
			return Entry.PlayerId == LocalPlayerId &&
				Entry.Status != EWeaponRecoveryPlayerStatus::Departed;
		});
	if (!LocalRosterEntry)
	{
		FailLocalSaveFork(TEXT("The current OSS identity does not own a live Slot in the local SaveGame"));
		return;
	}

	LocalForkSourceMatchId = SaveGame->Snapshot.MatchId;
	SaveGame->Snapshot.MatchId = FGuid::NewGuid();
	TArray<uint8> SerializedBytes;
	if (!UWeaponSnapshotCoordinator::ValidateSaveGame(SaveGame, Error) ||
		!UWeaponSnapshotCoordinator::SerializeSaveGame(SaveGame, SerializedBytes, Error))
	{
		FailLocalSaveFork(Error);
		return;
	}

	PendingLocalForkDescriptor = FWeaponRecoverySessionDescriptor();
	PendingLocalForkDescriptor.MatchId = SaveGame->Snapshot.MatchId;
	PendingLocalForkDescriptor.AuthorityEpoch = 1;
	PendingLocalForkDescriptor.BaseSnapshotRevision = SaveGame->Snapshot.Revision;
	PendingLocalForkDescriptor.BaseRosterRevision = SaveGame->Snapshot.RosterRevision;
	PendingLocalForkDescriptor.RecoveryAttemptId = FGuid::NewGuid();
	PendingLocalForkDescriptor.Candidate.PlayerId = LocalPlayerId;
	PendingLocalForkDescriptor.Candidate.OriginalSlot = LocalRosterEntry->OriginalSlot;
	PendingLocalForkDescriptor.MapPackageName = SaveGame->MapPackageName;
	PendingLocalForkDescriptor.MaxPlayers = SaveGame->MaxPlayers;
	PendingLocalForkDescriptor.State = EWeaponRecoverySessionState::Restoring;
	if (!ComputePayloadHash(SerializedBytes, PendingLocalForkDescriptor.PayloadHash) ||
		!PendingLocalForkDescriptor.IsValid(true))
	{
		FailLocalSaveFork(TEXT("The re-identified local SaveGame could not form a valid fork descriptor"));
		return;
	}

	PendingLocalForkSaveGame = SaveGame;
	SetMigrationState(
		EWeaponHostMigrationState::PreparingLocalFork,
		TEXT("Opening a clean gated listen World for the new-Match local SaveGame fork."),
		0.35f,
		true);
	UWeaponSessionSubsystem* MutableSessionSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	if (!MutableSessionSubsystem)
	{
		FailLocalSaveFork(TEXT("The Session subsystem could not prepare the local SaveGame fork World"));
		return;
	}
	if (!MutableSessionSubsystem->PrepareRecoveryHost(PendingLocalForkDescriptor) &&
		MigrationState == EWeaponHostMigrationState::PreparingLocalFork)
	{
		FailLocalSaveFork(TEXT("The Session subsystem could not prepare the local SaveGame fork World"));
	}
}

bool UWeaponHostMigrationSubsystem::RestoreLocalSaveFork(FString& OutError)
{
	OutError.Reset();
	UWorld* World = GetWorld();
	AWeaponGameMode* GameMode = World ? World->GetAuthGameMode<AWeaponGameMode>() : nullptr;
	AWeaponGameState* GameState = World ? World->GetGameState<AWeaponGameState>() : nullptr;
	UWeaponSnapshotCoordinator* Coordinator =
		GetGameInstance()->GetSubsystem<UWeaponSnapshotCoordinator>();
	if (!World || World->GetNetMode() != NM_ListenServer || !GameMode || !GameState ||
		!GameState->IsGameplayRecoveryBlocked() || !Coordinator || !PendingLocalForkSaveGame ||
		PendingLocalForkSaveGame->Snapshot.MatchId != PendingLocalForkDescriptor.MatchId ||
		!Coordinator->RestoreSaveGame(PendingLocalForkSaveGame, OutError))
	{
		if (OutError.IsEmpty())
		{
			OutError = TEXT("The local SaveGame fork World or restore payload is invalid");
		}
		return false;
	}

	SetMigrationState(
		EWeaponHostMigrationState::AdvertisingLocalFork,
		TEXT("Local state restored under a new MatchId. Advertising the new LAN authority."),
		0.8f,
		true);
	UWeaponSessionSubsystem* SessionSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	if (!SessionSubsystem)
	{
		OutError = TEXT("The restored local SaveGame fork could not start LAN advertisement");
		return false;
	}
	if (!SessionSubsystem->HostLanGame(
		PendingLocalForkSaveGame->MaxPlayers,
		PendingLocalForkServerName) &&
		MigrationState == EWeaponHostMigrationState::AdvertisingLocalFork)
	{
		OutError = TEXT("The restored local SaveGame fork could not start LAN advertisement");
		return false;
	}
	return true;
}

void UWeaponHostMigrationSubsystem::HandleLanHostComplete(
	const bool bSuccess,
	const FString& Message)
{
	if (MigrationState != EWeaponHostMigrationState::AdvertisingLocalFork)
	{
		return;
	}
	if (!bSuccess)
	{
		FailLocalSaveFork(Message);
		return;
	}

	AWeaponGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AWeaponGameMode>() : nullptr;
	UWeaponSnapshotCoordinator* Coordinator =
		GetGameInstance()->GetSubsystem<UWeaponSnapshotCoordinator>();
	if (!GameMode || !Coordinator || !PendingLocalForkSaveGame ||
		Coordinator->GetOrCreateActiveMatchId() != PendingLocalForkSaveGame->Snapshot.MatchId ||
		!GameMode->ReleaseGameplayRecoveryBarrier())
	{
		FailLocalSaveFork(TEXT("The new LAN Session was created but its fork barrier could not be released"));
		return;
	}

	UE_LOG(
		LogLetsDeepRock,
		Warning,
		TEXT("Local Host SaveGame forked from Match %s to new Match %s; the old MatchId will not be advertised"),
		*LocalForkSourceMatchId.ToString(),
		*PendingLocalForkSaveGame->Snapshot.MatchId.ToString());
	PendingLocalForkSaveGame = nullptr;
	PendingLocalForkDescriptor = FWeaponRecoverySessionDescriptor();
	LocalForkSourceMatchId.Invalidate();
	PendingLocalForkServerName.Reset();
	SetMigrationState(
		EWeaponHostMigrationState::Monitoring,
		TEXT("Local SaveGame fork is live under a new MatchId."),
		1.0f,
		false);
}

void UWeaponHostMigrationSubsystem::FailLocalSaveFork(const FString& Reason)
{
	const FName LobbyMap = PendingLocalForkDescriptor.MapPackageName.IsEmpty()
		? NAME_None
		: FName(*PendingLocalForkDescriptor.MapPackageName);
	UE_LOG(
		LogLetsDeepRock,
		Error,
		TEXT("Local SaveGame fork failed SourceMatch=%s NewMatch=%s: %s"),
		*LocalForkSourceMatchId.ToString(),
		*PendingLocalForkDescriptor.MatchId.ToString(),
		*Reason);
	PendingLocalForkSaveGame = nullptr;
	PendingLocalForkDescriptor = FWeaponRecoverySessionDescriptor();
	LocalForkSourceMatchId.Invalidate();
	PendingLocalForkServerName.Reset();
	SetMigrationState(
		EWeaponHostMigrationState::Failed,
		FString::Printf(TEXT("Local SaveGame fork failed: %s"), *Reason),
		0.0f,
		true);
	if (!LobbyMap.IsNone())
	{
		if (UWeaponSessionSubsystem* SessionSubsystem =
			GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>())
		{
			SessionSubsystem->ReturnToRecoveryLobby(LobbyMap);
		}
	}
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
	CommonCommittedRoleAssignment = CurrentRoleAssignment;
	bHasCommonCommittedRoleAssignment = true;
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

bool UWeaponHostMigrationSubsystem::RecordCommonCommittedConfirmation(
	const AWeaponPlayerController* Sender,
	const FWeaponMigrationPayloadAck& Ack,
	FString& OutError)
{
	OutError.Reset();
	const bool bFromBackupA = Sender && Sender == HostBackupA.Get();
	const bool bFromBackupB = Sender && Sender == HostBackupB.Get();
	const APlayerState* SenderPlayerState = Sender
		? Sender->GetPlayerState<APlayerState>()
		: nullptr;
	const EWeaponMigrationRole SenderRole = SenderPlayerState
		? GetRoleForPlayer(SenderPlayerState->GetUniqueId())
		: EWeaponMigrationRole::None;
	if (!bHasCommonCommittedPayload || !Ack.IdentifiesSamePayload(CommonCommittedAck) ||
		(!bFromBackupA && !bFromBackupB) ||
		(bFromBackupA && SenderRole != EWeaponMigrationRole::BackupA) ||
		(bFromBackupB && SenderRole != EWeaponMigrationRole::BackupB))
	{
		OutError = TEXT("CommonCommitted confirmation is stale or not from the fenced Backup A/B");
		return false;
	}

	if (bFromBackupA)
	{
		bBackupAConfirmedCommonCommitted = true;
	}
	if (bFromBackupB)
	{
		bBackupBConfirmedCommonCommitted = true;
	}
	if (!bBackupAConfirmedCommonCommitted || !bBackupBConfirmedCommonCommitted)
	{
		return true;
	}

	UE_LOG(
		LogLetsDeepRock,
		Log,
		TEXT("CommonCommitted converged on Backup A/B Match=%s Epoch=%llu Snapshot=%lld Roster=%llu"),
		*Ack.MatchId.ToString(),
		Ack.AuthorityEpoch,
		Ack.SnapshotRevision,
		Ack.RosterRevision);
	if (MigrationState == EWeaponHostMigrationState::PlannedHandoff &&
		Ack.SnapshotRevision == PlannedHandoffTargetRevision)
	{
		CompletePlannedHostHandoff(
			true,
			TEXT("Backup A and Backup B pinned and confirmed the final CommonCommitted Snapshot."));
	}
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
	CommonCommittedPayload = FCommittedPayloadBuffer();
}

bool UWeaponHostMigrationSubsystem::IsPendingPayloadComplete() const
{
	return PendingPayload.bActive &&
		PendingPayload.ReceivedChunkCount == PendingPayload.Envelope.ChunkCount &&
		PendingPayload.ReceivedByteCount == PendingPayload.Envelope.PayloadByteCount;
}

void UWeaponHostMigrationSubsystem::GetPendingPayloadProgress(
	int32& OutReceivedChunks,
	int32& OutTotalChunks) const
{
	if (!PendingPayload.bActive)
	{
		OutReceivedChunks = 0;
		OutTotalChunks = 0;
		return;
	}
	OutReceivedChunks = PendingPayload.ReceivedChunkCount;
	OutTotalChunks = PendingPayload.Chunks.Num();
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

bool UWeaponHostMigrationSubsystem::CopyCommonCommittedPayload(TArray<uint8>& OutBytes) const
{
	OutBytes.Reset();
	if (!CommonCommittedPayload.bValid)
	{
		return false;
	}
	OutBytes = CommonCommittedPayload.Bytes;
	return true;
}

bool UWeaponHostMigrationSubsystem::GetLocalCommonCommittedAck(
	FWeaponMigrationPayloadAck& OutAck) const
{
	OutAck = FWeaponMigrationPayloadAck();
	if (!CommonCommittedPayload.bValid)
	{
		return false;
	}
	OutAck.MatchId = CommonCommittedPayload.Envelope.MatchId;
	OutAck.AuthorityEpoch = CommonCommittedPayload.Envelope.AuthorityEpoch;
	OutAck.SnapshotRevision = CommonCommittedPayload.Envelope.SnapshotRevision;
	OutAck.RosterRevision = CommonCommittedPayload.Envelope.RosterRevision;
	OutAck.PayloadHash = CommonCommittedPayload.Envelope.PayloadHash;
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

	TUniquePtr<FEncryptionContext> EncryptionContext =
		IPlatformCrypto::Get().CreateContext();
	if (!EncryptionContext.IsValid() ||
		!EncryptionContext->CalcSHA256(PayloadBytes, OutHash) ||
		OutHash.Num() != WeaponMigrationSHA256Bytes)
	{
		OutHash.Reset();
		return false;
	}
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
		if (MigrationState == EWeaponHostMigrationState::PlannedHandoff)
		{
			CancelPlannedHostHandoff(TEXT("The final local SaveGame write failed"));
		}
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
		if (MigrationState == EWeaponHostMigrationState::PlannedHandoff)
		{
			CompletePlannedHostHandoff(
				false,
				TEXT("Final transport became unavailable before CommonCommitted convergence."));
		}
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
	if (MigrationState == EWeaponHostMigrationState::PlannedHandoff)
	{
		CompletePlannedHostHandoff(
			false,
			TEXT("Final A/B payload ACK timed out before CommonCommitted convergence."));
	}
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
