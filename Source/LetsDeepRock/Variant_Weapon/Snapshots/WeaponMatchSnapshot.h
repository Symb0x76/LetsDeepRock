#pragma once

#include "CoreMinimal.h"
#include "WeaponGameState.h"
#include "WeaponMatchSnapshot.generated.h"

/** Snapshot contract version. Increment only when the serialized layout changes. */
inline constexpr int32 WeaponMatchSnapshotVersion = 1;

/** State currently owned by one connected player. */
USTRUCT()
struct LETSDEEPROCK_API FWeaponPlayerSnapshot
{
	GENERATED_BODY()

	/** Temporary identity fallback. Replace the player name with an OSS account ID before migration ships. */
	UPROPERTY(SaveGame)
	FString PlayerId;

	UPROPERTY(SaveGame)
	FString PlayerName;

	UPROPERTY(SaveGame)
	int32 SlotIndex = INDEX_NONE;

	UPROPERTY(SaveGame)
	bool bIsReady = false;

	/** Lobby players may not have a Pawn yet. */
	UPROPERTY(SaveGame)
	bool bHasPawn = false;

	UPROPERTY(SaveGame)
	FVector Location = FVector::ZeroVector;

	UPROPERTY(SaveGame)
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY(SaveGame)
	FVector AimDirection = FVector(1.0f, 0.0f, 0.0f);

	UPROPERTY(SaveGame)
	FVector Velocity = FVector::ZeroVector;
};

/** Minimal state needed to respawn one living enemy. */
USTRUCT()
struct LETSDEEPROCK_API FWeaponEnemySnapshot
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FSoftClassPath EnemyClass;

	UPROPERTY(SaveGame)
	FVector Location = FVector::ZeroVector;

	UPROPERTY(SaveGame)
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY(SaveGame)
	FVector Velocity = FVector::ZeroVector;
};

/**
 * First checkpoint contract: only facts that the current Coordinator can capture.
 * New domains should be added after their owner exposes a tested Capture/Restore pair.
 */
USTRUCT()
struct LETSDEEPROCK_API FWeaponMatchSnapshot
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	int32 SchemaVersion = WeaponMatchSnapshotVersion;

	/** Stable for the lifetime of one match, including server travel. */
	UPROPERTY(SaveGame)
	FGuid MatchId;

	/** Monotonically increasing inside one MatchId. */
	UPROPERTY(SaveGame)
	int64 Revision = 0;

	UPROPERTY(SaveGame)
	FString MapPackageName;

	UPROPERTY(SaveGame)
	int32 MaxPlayers = 4;

	UPROPERTY(SaveGame)
	EWeaponMatchPhase MatchPhase = EWeaponMatchPhase::Waiting;

	/** Rebuild the destination server's deadline from this relative duration. */
	UPROPERTY(SaveGame)
	float RemainingPhaseTime = 0.0f;

	UPROPERTY(SaveGame)
	TArray<FWeaponPlayerSnapshot> Players;

	UPROPERTY(SaveGame)
	TArray<FWeaponEnemySnapshot> Enemies;
};
