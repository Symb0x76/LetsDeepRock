// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Engine/EngineBaseTypes.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "WeaponHostMigrationTypes.h"
#include "WeaponHostMigrationSubsystem.generated.h"

class UWeaponMatchSaveGame;
class AWeaponPlayerController;
class UWeaponSessionSubsystem;
class UWorld;
class UNetDriver;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(
	FOnWeaponMigrationPresentationChanged,
	bool, bVisible,
	FText, Message,
	float, Progress);

/**
 * Cross-travel owner of one backup's recovery bytes.
 *
 * Pending may be replaced or discarded while chunks arrive. Committed changes only after the
 * assembled bytes pass SHA-256, SaveGame deserialization and pure Snapshot validation.
 * PlayerController RPCs carry chunks and ACKs because this subsystem is not replicated. The
 * subsystem owns cross-travel roles, Epoch fencing, pacing, timeout policy, A/B ACK aggregation
 * and the local Host SaveGame. GameMode derives A/B from the Ready roster; recovery-session
 * orchestration decides when an assigned candidate may begin its prepared successor term.
 */
UCLASS(Config = Game, DefaultConfig)
class LETSDEEPROCK_API UWeaponHostMigrationSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;
	EWeaponHostMigrationState GetMigrationState() const { return MigrationState; }

	/** Freeze the current listen match, commit one final payload to A/B, then join the successor. */
	UFUNCTION(BlueprintCallable, Category = "Host Migration")
	bool RequestPlannedHostHandoff();

	/** Explicit split-brain-safe local restart: the loaded SaveGame always receives a new MatchId. */
	UFUNCTION(BlueprintCallable, Category = "Host Migration")
	bool RestartLatestLocalSaveAsNewMatch(const FString& ServerName);

	UFUNCTION(BlueprintPure, Category = "Host Migration")
	bool IsMigrationOverlayVisible() const { return bMigrationOverlayVisible; }

	UFUNCTION(BlueprintPure, Category = "Host Migration")
	FText GetMigrationOverlayMessage() const { return MigrationOverlayMessage; }

	UFUNCTION(BlueprintPure, Category = "Host Migration")
	float GetMigrationProgress() const { return MigrationProgress; }

	UPROPERTY(BlueprintAssignable, Category = "Host Migration")
	FOnWeaponMigrationPresentationChanged OnMigrationPresentationChanged;

	/** Create Epoch 1 for a new MatchId, or reuse the term of the same Active Host. */
	bool PrepareAuthorityTerm(
		const FGuid& MatchId,
		const FWeaponMigrationRoleMember& ActiveHost,
		uint64 RosterRevision,
		uint64& OutAuthorityEpoch,
		FString& OutError);

	/** Prepare the deterministic A=old+1 or B=old+2 candidate term without publishing it. */
	bool BeginCandidateAuthorityTerm(
		const FUniqueNetIdRepl& CandidatePlayerId,
		uint64 ExpectedAuthorityEpoch,
		uint64& OutCandidateEpoch,
		FString& OutError);

	/** Install the complete assignment computed from the authoritative Ready roster. */
	bool SetAuthorityRoleAssignment(
		const FWeaponMigrationRoleAssignment& Assignment,
		FString& OutError);

	/** Accept a Host-published assignment only when Match/Epoch ordering is valid. */
	bool AcceptRoleAssignment(
		const FWeaponMigrationRoleAssignment& Assignment,
		FString& OutError);

	/** Accept the Host's proof that both backups committed one identical payload. */
	bool AcceptCommonCommittedAnnouncement(
		const FUniqueNetIdRepl& ReceiverPlayerId,
		const FWeaponMigrationPayloadAck& Ack,
		FString& OutError);

	const FWeaponMigrationRoleAssignment* GetCurrentRoleAssignment() const
	{
		return bHasCurrentRoleAssignment ? &CurrentRoleAssignment : nullptr;
	}
	bool IsCurrentRoleAssignmentProvisional() const { return bCurrentRoleAssignmentIsProvisional; }
	EWeaponMigrationRole GetRoleForPlayer(const FUniqueNetIdRepl& PlayerId) const;

	/** Begin periodic Host capture for externally selected remote Backup A/B controllers. */
	bool StartHostSnapshotTransport(
		AWeaponPlayerController* BackupA,
		AWeaponPlayerController* BackupB,
		uint64 SuppliedAuthorityEpoch,
		FString& OutError);

	void StopHostSnapshotTransport();
	bool IsHostSnapshotTransportConfigured(
		const AWeaponPlayerController* BackupA,
		const AWeaponPlayerController* BackupB,
		uint64 AuthorityEpoch) const;

	/** Capture, validate and serialize one revision; local async save completes before chunk send. */
	bool CaptureAndDistributeSnapshot(FString& OutError);

	/** Backup receive entry used by its owning PlayerController Client RPC. */
	bool ReceivePayloadChunk(
		const FUniqueNetIdRepl& ReceiverPlayerId,
		const FWeaponMigrationPayloadEnvelope& Envelope,
		const TArray<uint8>& ChunkBytes,
		bool& bOutAckReady,
		FWeaponMigrationPayloadAck& OutAck,
		FString& OutError);

	/** Host ACK entry used by the sender's owning PlayerController Server RPC. */
	bool RecordBackupAcknowledgement(
		const AWeaponPlayerController* Sender,
		const FWeaponMigrationPayloadAck& Ack,
		FString& OutError);

	/** Backup confirmation that the Host's CommonCommitted announcement was accepted and pinned. */
	bool RecordCommonCommittedConfirmation(
		const AWeaponPlayerController* Sender,
		const FWeaponMigrationPayloadAck& Ack,
		FString& OutError);

	bool GetCommonCommittedAck(FWeaponMigrationPayloadAck& OutAck) const;
	int64 GetCommonCommittedRevision() const
	{
		return bHasCommonCommittedPayload ? CommonCommittedAck.SnapshotRevision : 0;
	}
	int64 GetLastLocalSaveRevision() const { return LastLocalSaveRevision; }

	/** Build the common envelope for a locally serialized, already validated SaveGame. */
	bool BuildPayloadEnvelope(
		const UWeaponMatchSaveGame* SaveGame,
		const TArray<uint8>& PayloadBytes,
		uint64 AuthorityEpoch,
		FWeaponMigrationPayloadEnvelope& OutEnvelope,
		FString& OutError) const;

	/** Store one chunk. Identical duplicates succeed; conflicting duplicates discard Pending. */
	bool StorePendingChunk(
		const FWeaponMigrationPayloadEnvelope& Envelope,
		const TArray<uint8>& ChunkBytes,
		bool& bOutPayloadComplete,
		FString& OutError);

	/** Promote a complete Pending payload without modifying the previous Committed on failure. */
	bool PromotePendingPayload(FWeaponMigrationPayloadAck& OutAck, FString& OutError);

	void DiscardPendingPayload();
	void ResetPayloadBuffers();

	bool HasPendingPayload() const { return PendingPayload.bActive; }
	bool IsPendingPayloadComplete() const;
	bool HasCommittedPayload() const { return CommittedPayload.bValid; }

	/** Debug/UI: 当前 Pending payload 分块接收进度;无 Pending 时返回 (0, 0)。 */
	void GetPendingPayloadProgress(int32& OutReceivedChunks, int32& OutTotalChunks) const;

	const FWeaponMigrationPayloadEnvelope* GetCommittedEnvelope() const;
	bool CopyCommittedPayload(TArray<uint8>& OutBytes) const;
	bool GetCommittedAck(FWeaponMigrationPayloadAck& OutAck) const;
	bool CopyCommonCommittedPayload(TArray<uint8>& OutBytes) const;
	bool GetLocalCommonCommittedAck(FWeaponMigrationPayloadAck& OutAck) const;

	/** Client-side acceptance of one fenced presentation-ready request. */
	bool AcceptRecoveryReadyRequest(
		const FGuid& MatchId,
		uint64 AuthorityEpoch,
		uint64 RosterRevision,
		const FGuid& RecoveryAttemptId,
		FString& OutError);

	/** Server-side ACK entry used by an owning PlayerController. */
	bool RecordRecoveryReadyAcknowledgement(
		const AWeaponPlayerController* Sender,
		const FGuid& MatchId,
		uint64 AuthorityEpoch,
		uint64 RosterRevision,
		const FGuid& RecoveryAttemptId,
		FString& OutError);

	float GetSnapshotIntervalSeconds() const { return FMath::Max(0.1f, SnapshotIntervalSeconds); }
	float GetTransferAckTimeoutSeconds() const { return FMath::Max(0.1f, TransferAckTimeoutSeconds); }
	float GetChunkAssemblyTimeoutSeconds() const { return FMath::Max(0.1f, ChunkAssemblyTimeoutSeconds); }
	float GetCandidateRecoveryTimeoutSeconds() const { return FMath::Max(0.1f, CandidateRecoveryTimeoutSeconds); }
	int32 GetMaxSnapshotBytes() const { return FMath::Max(1, MaxSnapshotBytes); }
	int32 GetChunkPayloadBytes() const { return FMath::Clamp(ChunkPayloadBytes, 1, GetMaxSnapshotBytes()); }
	const FString& GetLocalHostSaveSlotName() const { return LocalHostSaveSlotName; }
	int32 GetLocalHostSaveUserIndex() const { return FMath::Max(0, LocalHostSaveUserIndex); }

private:
	struct FPendingPayloadBuffer
	{
		FWeaponMigrationPayloadEnvelope Envelope;
		TArray<TArray<uint8>> Chunks;
		TBitArray<> ReceivedChunks;
		int32 ReceivedChunkCount = 0;
		int32 ReceivedByteCount = 0;
		bool bActive = false;
	};

	struct FCommittedPayloadBuffer
	{
		FWeaponMigrationPayloadEnvelope Envelope;
		TArray<uint8> Bytes;
		bool bValid = false;
	};

	struct FOutgoingPayloadTransfer
	{
		FWeaponMigrationPayloadEnvelope Envelope;
		TArray<uint8> Bytes;
		int32 NextChunkIndex = 0;
		bool bBackupAAcked = false;
		bool bBackupBAcked = false;
		bool bWaitingForLocalSave = false;
		bool bActive = false;
	};

	bool ValidateEnvelopeShape(
		const FWeaponMigrationPayloadEnvelope& Envelope,
		int32 ChunkByteCount,
		FString& OutError) const;
	void InitializePendingPayload(const FWeaponMigrationPayloadEnvelope& Envelope);
	static bool ComputePayloadHash(const TArray<uint8>& PayloadBytes, TArray<uint8>& OutHash);
	static bool HashesEqual(const TArray<uint8>& A, const TArray<uint8>& B);
	static bool ValidateRoleAssignment(
		const FWeaponMigrationRoleAssignment& Assignment,
		FString& OutError);
	bool ValidateCurrentEnvelope(
		const FWeaponMigrationPayloadEnvelope& Envelope,
		FString& OutError) const;
	bool IsHostTransportReady(FString& OutError) const;
	void HandleSnapshotInterval();
	void HandleLocalSaveComplete(uint64 RequestSerial, bool bSuccess);
	void SendNextOutgoingChunk();
	void HandleTransferAckTimeout();
	void HandleChunkAssemblyTimeout();
	void ResetOutgoingTransfer();
	void ClearHostTimers();
	void BroadcastCommonCommittedAck();
	void HandleNetworkFailure(
		UWorld* World,
		UNetDriver* NetDriver,
		ENetworkFailure::Type FailureType,
		const FString& ErrorString);
	void HandlePostLoadMap(UWorld* LoadedWorld);
	void HandleRecoveryWorldPrepared(bool bSuccess, const FString& Message);
	void HandleRecoveryHostComplete(bool bSuccess, const FString& Message);
	void HandleRecoveryJoinComplete(bool bSuccess, const FString& Message);
	void HandleLanHostComplete(bool bSuccess, const FString& Message);
	bool TickRecovery(float DeltaSeconds);
	bool PrimeRecoveryFromCommittedBaseline(UWorld* SourceWorld, FString& OutError);
	bool EnterRecoveryCandidatePhase(EWeaponMigrationRole CandidateRole, FString& OutError);
	bool BuildRecoverySessionDescriptor(
		EWeaponMigrationRole CandidateRole,
		bool bCreateAttempt,
		FWeaponRecoverySessionDescriptor& OutDescriptor,
		FString& OutError) const;
	bool RestoreCandidateWorld(FString& OutError);
	bool RefreshRecoveryReadyFence(FString& OutError);
	bool TryReleaseRecoveryBarrier(FString& OutError);
	void RestartRecoveryCandidateAttempt(const FString& Reason);
	void AdvanceRecoveryCandidate(const FString& Reason);
	void FailRecoveryToLobby(const FString& Reason);
	void CompleteRecoveryConnection();
	void ResetRecoveryAttemptContext();
	void SetMigrationState(
		EWeaponHostMigrationState NewState,
		const FString& Message,
		float Progress,
		bool bShowOverlay);
	static const TCHAR* LexMigrationState(EWeaponHostMigrationState State);
	void CompletePlannedHostHandoff(bool bFinalRevisionCommitted, const FString& Reason);
	void CancelPlannedHostHandoff(const FString& Reason);
	void HandleLocalForkSaveLoaded(uint64 RequestSerial, class USaveGame* LoadedSaveGame);
	bool RestoreLocalSaveFork(FString& OutError);
	void FailLocalSaveFork(const FString& Reason);

	UPROPERTY(Config, EditDefaultsOnly, Category = "Host Migration|Timing", meta = (ClampMin = 0.1))
	float SnapshotIntervalSeconds = 5.0f;

	UPROPERTY(Config, EditDefaultsOnly, Category = "Host Migration|Timing", meta = (ClampMin = 0.1))
	float TransferAckTimeoutSeconds = 10.0f;

	UPROPERTY(Config, EditDefaultsOnly, Category = "Host Migration|Timing", meta = (ClampMin = 0.1))
	float ChunkAssemblyTimeoutSeconds = 10.0f;

	UPROPERTY(Config, EditDefaultsOnly, Category = "Host Migration|Timing", meta = (ClampMin = 0.1))
	float CandidateRecoveryTimeoutSeconds = 60.0f;

	UPROPERTY(Config, EditDefaultsOnly, Category = "Host Migration|Payload", meta = (ClampMin = 1))
	int32 MaxSnapshotBytes = 4194304;

	UPROPERTY(Config, EditDefaultsOnly, Category = "Host Migration|Payload", meta = (ClampMin = 1))
	int32 ChunkPayloadBytes = 32768;

	UPROPERTY(Config, EditDefaultsOnly, Category = "Host Migration|Local Save")
	FString LocalHostSaveSlotName = TEXT("WeaponHostMigrationLatest");

	UPROPERTY(Config, EditDefaultsOnly, Category = "Host Migration|Local Save", meta = (ClampMin = 0))
	int32 LocalHostSaveUserIndex = 0;

	FPendingPayloadBuffer PendingPayload;
	FCommittedPayloadBuffer CommittedPayload;
	FCommittedPayloadBuffer CommonCommittedPayload;
	FOutgoingPayloadTransfer OutgoingTransfer;
	FWeaponMigrationPayloadAck CommonCommittedAck;
	FWeaponMigrationRoleAssignment CommonCommittedRoleAssignment;
	FWeaponMigrationRoleAssignment CurrentRoleAssignment;
	TWeakObjectPtr<AWeaponPlayerController> HostBackupA;
	TWeakObjectPtr<AWeaponPlayerController> HostBackupB;
	TWeakObjectPtr<UWorld> HostWorld;
	uint64 HostAuthorityEpoch = 0;
	uint64 LocalSaveRequestSerial = 0;
	int64 LastLocalSaveRevision = 0;
	bool bHasCommonCommittedPayload = false;
	bool bHasCommonCommittedRoleAssignment = false;
	bool bBackupAConfirmedCommonCommitted = false;
	bool bBackupBConfirmedCommonCommitted = false;
	bool bHasCurrentRoleAssignment = false;
	bool bCurrentRoleAssignmentIsProvisional = false;
	EWeaponHostMigrationState MigrationState = EWeaponHostMigrationState::Idle;
	EWeaponMigrationRole RecoveryCandidateRole = EWeaponMigrationRole::None;
	FWeaponMigrationRoleAssignment RecoveryOriginAssignment;
	FWeaponMigrationPayloadAck RecoveryBaselineAck;
	FWeaponRecoverySessionDescriptor ActiveRecoverySession;
	FUniqueNetIdRepl RecoveryLocalPlayerId;
	double RecoveryCandidateDeadlineSeconds = 0.0;
	double NextRecoverySearchSeconds = 0.0;
	bool bRecoverySearchInFlight = false;
	bool bRecoveryTravelCompleted = false;
	bool bRecoveryReadyRequestReceived = false;
	uint64 RecoveryPublicationRosterRevision = 0;
	uint64 RecoveryReadyRosterRevision = 0;
	TSet<FUniqueNetIdRepl> RecoveryReadyPlayerAcks;
	int64 PlannedHandoffTargetRevision = 0;
	double PlannedHandoffDeadlineSeconds = 0.0;
	uint64 LocalForkRequestSerial = 0;
	FGuid LocalForkSourceMatchId;
	FString PendingLocalForkServerName;
	FWeaponRecoverySessionDescriptor PendingLocalForkDescriptor;

	UPROPERTY()
	TObjectPtr<UWeaponMatchSaveGame> PendingLocalForkSaveGame;

	UPROPERTY()
	bool bMigrationOverlayVisible = false;

	UPROPERTY()
	FText MigrationOverlayMessage;

	UPROPERTY()
	float MigrationProgress = 0.0f;

	FDelegateHandle NetworkFailureDelegateHandle;
	FDelegateHandle PostLoadMapDelegateHandle;
	FDelegateHandle RecoveryWorldPreparedDelegateHandle;
	FDelegateHandle RecoveryHostCompleteDelegateHandle;
	FDelegateHandle RecoveryJoinCompleteDelegateHandle;
	FDelegateHandle LanHostCompleteDelegateHandle;
	FTSTicker::FDelegateHandle RecoveryTickerHandle;
	FTimerHandle SnapshotCaptureTimerHandle;
	FTimerHandle ChunkSendTimerHandle;
	FTimerHandle TransferAckTimerHandle;
	FTimerHandle ChunkAssemblyTimerHandle;
};
