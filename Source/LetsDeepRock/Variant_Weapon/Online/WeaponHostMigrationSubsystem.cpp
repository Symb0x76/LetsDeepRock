// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponHostMigrationSubsystem.h"
#include "WeaponMatchSaveGame.h"
#include "WeaponSnapshot.h"
#include "WeaponSnapshotCoordinator.h"
#include "HAL/PlatformMisc.h"

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
