#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "WeaponGameState.h"
#include "WeaponSnapshot.generated.h"

class UGameplayEffect;
class UWeaponDataAsset;
/** Snapshot contract version. Increment only when the serialized layout changes. */
inline constexpr int32 WeaponSnapshotVersion = 1;

USTRUCT()
struct FWeaponAttributeState
{
	GENERATED_BODY()

	// 所有需要快照的属性值，key 为 GameplayTag（如 "Attr.Health"）
	// Capture 时遍历 AttributeSet 获取当前值，Restore 时 SetNumericAttributeBase 写入
	UPROPERTY(SaveGame)
	TMap<FGameplayTag, float> Values;
};

/** State currently owned by one connected player. */
USTRUCT()
struct LETSDEEPROCK_API FWeaponPlayerSnapshot
{
	GENERATED_BODY()

	/** Temporary identity fallback. Replace the player name with an OSS account ID before migration ships. */
	UPROPERTY(SaveGame)
	FString PlayerId;
	
	/* TODO: Determine whether to keep Player Name */
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
	int32 TeamId = 0;

	UPROPERTY(SaveGame)
	FVector Location = FVector::ZeroVector;

	UPROPERTY(SaveGame)
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY(SaveGame)
	FVector AimDirection = FVector(1.0f, 0.0f, 0.0f);

	UPROPERTY(SaveGame)
	FVector Velocity = FVector::ZeroVector;
	
	UPROPERTY(SaveGame)
	FName CharacterConfigRowName = NAME_None;
	
	UPROPERTY(SaveGame)
	TArray<TObjectPtr<UWeaponDataAsset>> WeaponInventory;

	UPROPERTY(SaveGame)
	int32 CurrentWeaponIndex = INDEX_NONE;

	UPROPERTY(SaveGame)
	FWeaponAttributeState AttributeState;
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
	FVector AimDirection = FVector(1.0f, 0.0f, 0.0f);

	UPROPERTY(SaveGame)
	FVector Velocity = FVector::ZeroVector;
	
	UPROPERTY(SaveGame)
	FName EnemyConfigRowName = NAME_None;

	UPROPERTY(SaveGame)
	FWeaponAttributeState AttributeState;
};

/**
 * First checkpoint contract: only facts that the current Coordinator can capture.
 * New domains should be added after their owner exposes a tested Capture/Restore pair.
 */
USTRUCT()
struct LETSDEEPROCK_API FWeaponSnapshot
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	int32 SchemaVersion = WeaponSnapshotVersion;

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
