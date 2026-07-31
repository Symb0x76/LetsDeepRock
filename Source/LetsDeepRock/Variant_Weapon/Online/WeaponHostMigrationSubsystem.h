// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "WeaponHostMigrationTypes.h"
#include "WeaponHostMigrationSubsystem.generated.h"

class UWeaponMatchSaveGame;
class AWeaponPlayerController;
class UWorld;

/**
 * Cross-travel owner of one backup's recovery bytes.
 *
 * Pending may be replaced or discarded while chunks arrive. Committed changes only after the
 * assembled bytes pass SHA-256, SaveGame deserialization and pure Snapshot validation.
 * PlayerController RPCs carry chunks and ACKs because this subsystem is not replicated. The
 * subsystem owns pacing, timeout policy, A/B ACK aggregation and the local Host SaveGame; fixed
 * role selection and authority-term election remain external.
 */
UCLASS(Config = Game, DefaultConfig)
class LETSDEEPROCK_API UWeaponHostMigrationSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Deinitialize() override;

	/** Begin periodic Host capture for externally selected remote Backup A/B controllers. */
	bool StartHostSnapshotTransport(
		AWeaponPlayerController* BackupA,
		AWeaponPlayerController* BackupB,
		uint64 SuppliedAuthorityEpoch,
		FString& OutError);

	void StopHostSnapshotTransport();

	/** Capture, validate and serialize one revision; local async save completes before chunk send. */
	bool CaptureAndDistributeSnapshot(FString& OutError);

	/** Backup receive entry used by its owning PlayerController Client RPC. */
	bool ReceivePayloadChunk(
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

	const FWeaponMigrationPayloadEnvelope* GetCommittedEnvelope() const;
	bool CopyCommittedPayload(TArray<uint8>& OutBytes) const;
	bool GetCommittedAck(FWeaponMigrationPayloadAck& OutAck) const;

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
	bool IsHostTransportReady(FString& OutError) const;
	void HandleSnapshotInterval();
	void HandleLocalSaveComplete(uint64 RequestSerial, bool bSuccess);
	void SendNextOutgoingChunk();
	void HandleTransferAckTimeout();
	void HandleChunkAssemblyTimeout();
	void ResetOutgoingTransfer();
	void ClearHostTimers();

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
	FOutgoingPayloadTransfer OutgoingTransfer;
	FWeaponMigrationPayloadAck CommonCommittedAck;
	TWeakObjectPtr<AWeaponPlayerController> HostBackupA;
	TWeakObjectPtr<AWeaponPlayerController> HostBackupB;
	TWeakObjectPtr<UWorld> HostWorld;
	uint64 HostAuthorityEpoch = 0;
	uint64 LocalSaveRequestSerial = 0;
	int64 LastLocalSaveRevision = 0;
	bool bHasCommonCommittedPayload = false;
	FTimerHandle SnapshotCaptureTimerHandle;
	FTimerHandle ChunkSendTimerHandle;
	FTimerHandle TransferAckTimerHandle;
	FTimerHandle ChunkAssemblyTimerHandle;
};
