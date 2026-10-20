#pragma once

#include "CoreMinimal.h"
#include "GameFramework/OnlineReplStructs.h"
#include "GameplayTagContainer.h"
#include "GridMap/GridMapTypes.h"
#include "Online/WeaponRecoveryRoster.h"
#include "WeaponGameState.h"
#include "WeaponSnapshot.generated.h"

/** Snapshot contract version. Increment only when the serialized layout changes. */
inline constexpr int32 WeaponSnapshotVersion = 9;

USTRUCT()
struct FWeaponAmmoSnapshot
{
	GENERATED_BODY()
	UPROPERTY(SaveGame) int32 MagazineAmmo = 0;
	UPROPERTY(SaveGame) int32 ReserveAmmo = 0;
};

USTRUCT()
struct FWeaponAttributeState
{
	GENERATED_BODY()

	// 所有需要快照的属性值，key 为 GameplayTag（如 "Attr.Health"）
	// Capture 时读 Base（GetNumericAttributeBase），Restore 时 SetNumericAttributeBase 写入；
	// GE 修饰器不在此处，由各自的 GE 快照/重新施加恢复。
	UPROPERTY(SaveGame)
	TMap<FGameplayTag, float> Values;
};

/** State currently owned by one connected player. */
USTRUCT()
struct LETSDEEPROCK_API FWeaponPlayerSnapshot
{
	GENERATED_BODY()

	/** Opaque OnlineSubsystem identity; PlayerName is presentation only. */
	UPROPERTY(SaveGame)
	FUniqueNetIdRepl PlayerId;
	
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
	FVector Velocity = FVector::ZeroVector;
	
	UPROPERTY(SaveGame)
	FName CharacterConfigRowName = NAME_None;

	UPROPERTY(SaveGame)
	FName ProfessionId = NAME_None;

	UPROPERTY(SaveGame)
	FName PrimaryWeaponId = NAME_None;

	UPROPERTY(SaveGame)
	FName SecondaryWeaponId = NAME_None;

	UPROPERTY(SaveGame)
	TArray<FSoftObjectPath> WeaponInventory;

	UPROPERTY(SaveGame)
	TArray<FWeaponAmmoSnapshot> WeaponAmmoStates;
	
	UPROPERTY(SaveGame)
	int32 CurrentWeaponIndex = INDEX_NONE;

	UPROPERTY(SaveGame)
	uint8 CurrentEquipmentSlot = 0;

	/** 仅记录诊断；恢复时统一取消换弹，因为没有可靠保存计时器剩余时间。 */
	UPROPERTY(SaveGame)
	bool bWasReloading = false;

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
	FVector Velocity = FVector::ZeroVector;
	
	UPROPERTY(SaveGame)
	FName EnemyConfigRowName = NAME_None;

	UPROPERTY(SaveGame)
	FWeaponAttributeState AttributeState;
};

/** 一枚仍在飞行的普通武器投射物。 */
USTRUCT()
struct LETSDEEPROCK_API FWeaponProjectileSnapshot
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FSoftClassPath ProjectileClass;

	UPROPERTY(SaveGame)
	FSoftClassPath DamageEffectClass;

	UPROPERTY(SaveGame)
	FUniqueNetIdRepl InstigatorPlayerId;

	UPROPERTY(SaveGame)
	FVector Location = FVector::ZeroVector;

	UPROPERTY(SaveGame)
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY(SaveGame)
	FVector Velocity = FVector::ZeroVector;

	UPROPERTY(SaveGame)
	FGameplayTagContainer DamageTags;

	UPROPERTY(SaveGame)
	FName StatusEffectId;

	UPROPERTY(SaveGame)
	float DamageValue = 0.0f;

	UPROPERTY(SaveGame)
	float ProjectileSpeed = 0.0f;

	UPROPERTY(SaveGame) float DamageFalloffStart = 0.0f;
	UPROPERTY(SaveGame) float DamageFalloffEnd = 0.0f;
	UPROPERTY(SaveGame) float ArmorPenetration = 0.0f;

	UPROPERTY(SaveGame) float AreaDamage = 0.0f;
	UPROPERTY(SaveGame) float AreaRadius = 0.0f;
	UPROPERTY(SaveGame) float AreaFalloff = 0.0f;
	UPROPERTY(SaveGame) float ProjectileGravityScale = 0.0f;
	UPROPERTY(SaveGame) int32 MaxHitTargets = 1;

	UPROPERTY(SaveGame)
	float RemainingLifeSeconds = 0.0f;

	UPROPERTY(SaveGame)
	bool bPenetratesTerrain = false;
};

/** 一枚仍未爆炸的手雷；静态爆炸配置由 DataAsset 重建。 */
USTRUCT()
struct LETSDEEPROCK_API FWeaponGrenadeSnapshot
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FSoftObjectPath GrenadeDataAsset;

	UPROPERTY(SaveGame)
	FUniqueNetIdRepl InstigatorPlayerId;

	UPROPERTY(SaveGame)
	FVector Location = FVector::ZeroVector;

	UPROPERTY(SaveGame)
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY(SaveGame)
	FVector Velocity = FVector::ZeroVector;

	UPROPERTY(SaveGame)
	float RemainingFuseTime = 0.0f;
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
	EWeaponMatchPhase MatchPhase = EWeaponMatchPhase::Waiting;

	/** Rebuild the destination server's deadline from this relative duration. */
	UPROPERTY(SaveGame)
	float RemainingPhaseTime = 0.0f;

	/** Complete server-owned participant set used by this Snapshot revision. */
	UPROPERTY(SaveGame)
	uint64 RosterRevision = 0;

	UPROPERTY(SaveGame)
	TArray<FWeaponRecoveryRosterSnapshot> RecoveryRoster;

	UPROPERTY(SaveGame)
	TArray<FWeaponPlayerSnapshot> Players;

	UPROPERTY(SaveGame)
	TArray<FWeaponEnemySnapshot> Enemies;

	UPROPERTY(SaveGame)
	FGridMapRecoveryState GridMap;

	UPROPERTY(SaveGame)
	TArray<FWeaponProjectileSnapshot> Projectiles;

	UPROPERTY(SaveGame)
	TArray<FWeaponGrenadeSnapshot> Grenades;
};
