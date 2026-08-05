// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/OnlineReplStructs.h"
#include "WeaponHostMigrationTypes.generated.h"

inline constexpr int32 WeaponMigrationProtocolVersion = 1;
inline constexpr int32 WeaponMigrationSHA256Bytes = 32;

UENUM()
enum class EWeaponMigrationRole : uint8
{
	None,
	ActiveHost,
	BackupA,
	BackupB,
};

/** Cross-World recovery orchestration state. This is an internal C++ protocol, not a Blueprint API. */
enum class EWeaponHostMigrationState : uint8
{
	Idle,
	Monitoring,
	WaitingForDisconnectTravel,
	PreparingCandidate,
	SearchingCandidate,
	JoiningCandidate,
	Restoring,
	AdvertisingCandidate,
	WaitingForReadyAcks,
	PlannedHandoff,
	LoadingLocalFork,
	PreparingLocalFork,
	RestoringLocalFork,
	AdvertisingLocalFork,
	Failed,
};

enum class EWeaponRecoverySessionState : uint8
{
	Restoring,
	Ready,
};

/** One stable player occupying one migration role for the current authority term. */
USTRUCT()
struct FWeaponMigrationRoleMember
{
	GENERATED_BODY()

	UPROPERTY()
	FUniqueNetIdRepl PlayerId;

	UPROPERTY()
	int32 OriginalSlot = INDEX_NONE;

	bool IsAssigned() const { return PlayerId.IsValid() && OriginalSlot != INDEX_NONE; }
	bool IdentifiesSameMember(const FWeaponMigrationRoleMember& Other) const;
};

/** Cross-travel role and fencing state published by the current Active Host. */
USTRUCT()
struct FWeaponMigrationRoleAssignment
{
	GENERATED_BODY()

	UPROPERTY()
	FGuid MatchId;

	UPROPERTY()
	uint64 AuthorityEpoch = 0;

	UPROPERTY()
	uint64 RosterRevision = 0;

	UPROPERTY()
	FWeaponMigrationRoleMember ActiveHost;

	UPROPERTY()
	FWeaponMigrationRoleMember BackupA;

	UPROPERTY()
	FWeaponMigrationRoleMember BackupB;

	EWeaponMigrationRole ResolveRole(const FUniqueNetIdRepl& PlayerId) const;
	bool IdentifiesSameAssignment(const FWeaponMigrationRoleAssignment& Other) const;
};

/** Session-discovery identity for one disposable candidate Recovery World. */
struct FWeaponRecoverySessionDescriptor
{
	FGuid MatchId;
	uint64 AuthorityEpoch = 0;
	int64 BaseSnapshotRevision = 0;
	uint64 BaseRosterRevision = 0;
	FGuid RecoveryAttemptId;
	FWeaponMigrationRoleMember Candidate;
	FString MapPackageName;
	int32 MaxPlayers = 0;
	TArray<uint8> PayloadHash;
	EWeaponRecoverySessionState State = EWeaponRecoverySessionState::Restoring;

	bool IsValid(bool bRequireAttemptId) const;
	bool IdentifiesSameBaseline(const FWeaponRecoverySessionDescriptor& Other) const;
};

/** Metadata shared by every chunk of one immutable serialized SaveGame payload. */
USTRUCT()
struct FWeaponMigrationPayloadEnvelope
{
	GENERATED_BODY()

	UPROPERTY()
	int32 ProtocolVersion = WeaponMigrationProtocolVersion;

	UPROPERTY()
	FGuid MatchId;

	UPROPERTY()
	uint64 AuthorityEpoch = 0;

	UPROPERTY()
	int64 SnapshotRevision = 0;

	UPROPERTY()
	uint64 RosterRevision = 0;

	UPROPERTY()
	int32 SaveFormatVersion = 0;

	UPROPERTY()
	int32 SnapshotSchemaVersion = 0;

	UPROPERTY()
	int32 PayloadByteCount = 0;

	UPROPERTY()
	int32 ChunkIndex = 0;

	UPROPERTY()
	int32 ChunkCount = 0;

	/** Exactly 32 raw SHA-256 bytes for the complete serialized payload. */
	UPROPERTY()
	TArray<uint8> PayloadHash;

	bool IdentifiesSamePayload(const FWeaponMigrationPayloadEnvelope& Other) const;
};

/** Emitted only after a complete payload has passed integrity and SaveGame validation. */
USTRUCT()
struct FWeaponMigrationPayloadAck
{
	GENERATED_BODY()

	UPROPERTY()
	FGuid MatchId;

	UPROPERTY()
	uint64 AuthorityEpoch = 0;

	UPROPERTY()
	int64 SnapshotRevision = 0;

	UPROPERTY()
	uint64 RosterRevision = 0;

	UPROPERTY()
	TArray<uint8> PayloadHash;

	bool IdentifiesPayload(const FWeaponMigrationPayloadEnvelope& Envelope) const;
	bool IdentifiesSamePayload(const FWeaponMigrationPayloadAck& Other) const;
};
