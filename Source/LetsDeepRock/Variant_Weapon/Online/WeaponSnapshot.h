#pragma once

#include "CoreMinimal.h"
#include "GameFramework/OnlineReplStructs.h"
#include "GameplayTagContainer.h"
#include "GridMap/GridMapTypes.h"
#include "Online/WeaponRecoveryRoster.h"
#include "Reinforcement/WeaponReinforcementRules.h"
#include "WeaponGameState.h"
#include "WeaponSnapshot.generated.h"

/** Snapshot contract version. Increment only when the serialized layout changes. */
inline constexpr int32 WeaponSnapshotVersion = 12;

/** v10:一条生效中的状态效果(GE 叠层 + 剩余时长 + 组件侧应用策略)。 */
USTRUCT()
struct FWeaponStatusEffectSnapshot
{
	GENERATED_BODY()
	UPROPERTY(SaveGame) FName StatusEffectId = NAME_None;
	UPROPERTY(SaveGame) int32 StackCount = 0;
	/** 相对时长;恢复时作为新 GE 的时长 */
	UPROPERTY(SaveGame) float RemainingDuration = 0.0f;
	UPROPERTY(SaveGame) int32 PendingHits = 0;
	UPROPERTY(SaveGame) FSoftClassPath DamageEffectClass;
};

/** v10:目标/撤离进度。时间一律存相对秒数,恢复后由屏障释放重新锚定。 */
USTRUCT()
struct FWeaponObjectiveSnapshot
{
	GENERATED_BODY()
	/** 本局是否已生成目标/撤离 Actor(InGame 之后才有) */
	UPROPERTY(SaveGame) bool bActorsSpawned = false;
	UPROPERTY(SaveGame) bool bObjectiveComplete = false;
	UPROPERTY(SaveGame) bool bExtractionOpen = false;
	/** 目标已完成、撤离尚未开启时,距离开启的剩余秒数 */
	UPROPERTY(SaveGame) float ExtractionRemainingSeconds = 0.0f;
	UPROPERTY(SaveGame) float ObjectiveChargeProgress = 0.0f;
	UPROPERTY(SaveGame) float ExtractionHoldProgress = 0.0f;
};

/** v11:某房间内某原型的存活虫群数(粗粒度,恢复时在房间刷怪点重新生成)。 */
USTRUCT()
struct FWeaponSwarmRoomCount
{
	GENERATED_BODY()
	/** RoomWaves 顺序下标:战斗房在前,防御波在 CombatRoomCount */
	UPROPERTY(SaveGame) int32 RoomIndex = INDEX_NONE;
	UPROPERTY(SaveGame) FName ArchetypeRow;
	UPROPERTY(SaveGame) int32 Count = 0;
};

/** v10:波次导演中的一个房间(含防御波)。 */
USTRUCT()
struct FWeaponWaveRoomSnapshot
{
	GENERATED_BODY()
	UPROPERTY(SaveGame) bool bTriggered = false;
	/** 精英(GAS 敌人)剩余配额 */
	UPROPERTY(SaveGame) int32 RemainingToSpawn = 0;
	/** v11:虫群剩余配额 */
	UPROPERTY(SaveGame) int32 RemainingSwarm = 0;
	UPROPERTY(SaveGame) float SecondsToNextSpawn = 0.0f;
};

/** v10:波次导演。战斗房按 Recipe 重建后按下标对齐;防御波单独记录中心点后重建。 */
USTRUCT()
struct FWeaponWaveDirectorSnapshot
{
	GENERATED_BODY()
	UPROPERTY(SaveGame) bool bArmed = false;
	UPROPERTY(SaveGame) int32 Difficulty = 2;
	UPROPERTY(SaveGame) TArray<FWeaponWaveRoomSnapshot> CombatRooms;
	UPROPERTY(SaveGame) bool bHasDefenseWave = false;
	UPROPERTY(SaveGame) FVector DefenseCenter = FVector::ZeroVector;
	UPROPERTY(SaveGame) FWeaponWaveRoomSnapshot DefenseWave;
	/** v11:各房间存活虫群计数 */
	UPROPERTY(SaveGame) TArray<FWeaponSwarmRoomCount> SwarmCounts;
};

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

	/** v10:死亡等待复活时为 true(RespawnRemainingSeconds 有效) */
	UPROPERTY(SaveGame)
	bool bRespawnPending = false;

	UPROPERTY(SaveGame)
	float RespawnRemainingSeconds = 0.0f;

	/** v12:生存状态;倒地时 DownedRemainingSeconds 为流血剩余秒数 */
	UPROPERTY(SaveGame)
	EWeaponPlayerLifeState LifeState = EWeaponPlayerLifeState::Up;

	UPROPERTY(SaveGame)
	float DownedRemainingSeconds = 0.0f;
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

	/** v10 */
	UPROPERTY(SaveGame)
	TArray<FWeaponStatusEffectSnapshot> StatusEffects;
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

	/** v10:任务参数。恢复世界不解析 travel options,以此为准(FWeaponMissionSettings 无 SaveGame 标记,故展开存) */
	UPROPERTY(SaveGame)
	int32 MissionSeed = 0;

	UPROPERTY(SaveGame)
	int32 MissionDifficulty = 1;

	/** v10 */
	UPROPERTY(SaveGame)
	FWeaponObjectiveSnapshot Objective;

	/** v10 */
	UPROPERTY(SaveGame)
	FWeaponWaveDirectorSnapshot WaveDirector;

	/** v12:增援池剩余次数与任务结局 */
	UPROPERTY(SaveGame)
	int32 ReinforcementPool = 0;

	UPROPERTY(SaveGame)
	EWeaponMissionOutcome MatchOutcome = EWeaponMissionOutcome::None;
};
