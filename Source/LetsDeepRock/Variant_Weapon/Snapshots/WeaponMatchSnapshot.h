/**
 * @Author: Symb0x76 9667434@qq.com
 * @Date: 2026-07-17 11:02:47
 * @LastEditors: Symb0x76 9667434@qq.com
 * @LastEditTime: 2026-07-20 12:56:38
 * @FilePath: Source/LetsDeepRock/Variant_Weapon/Snapshots/WeaponMatchSnapshot.h
 */

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "GridMap/GridMapTypes.h"
#include "WeaponGameState.h"
#include "WeaponMatchSnapshot.generated.h"

/**
 * 当前快照结构的设计版本。
 */
inline constexpr int32 WeaponMatchSnapshotVersion = 1;

/** 稳定实体 ID 的类型域，避免不同注册表中的字符串发生碰撞。 */
UENUM(BlueprintType)
enum class EWeaponStableEntityKind : uint8
{
	None,
	Player,
	Enemy,
	Projectile,
	WorldDrop,
	MutableActor,
	GridMap,
};

/**
 * 跨 World、跨进程引用实体时使用的纯数据身份。
 * Namespace 描述 ID 来源，例如 OSS、RuntimeGuid、AuthoredMap；Value 保存该来源中的稳定值。
 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponStableEntityId
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Identity")
	EWeaponStableEntityKind Kind = EWeaponStableEntityKind::None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Identity")
	FName Namespace = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Identity")
	FString Value;

	bool IsValid() const
	{
		return Kind != EWeaponStableEntityKind::None && Namespace != NAME_None && !Value.IsEmpty();
	}

	bool operator==(const FWeaponStableEntityId& Other) const
	{
		return Kind == Other.Kind && Namespace == Other.Namespace && Value == Other.Value;
	}
};

/** 用 GameplayTag 标识的浮点数，主要用于可扩展属性和 SetByCaller 数值。 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponTaggedFloatSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot")
	FGameplayTag Tag;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot")
	float Value = 0.0f;
};

/** 用名称标识的浮点数，适合生成器参数、组件计时和非 GameplayTag 数值。 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponNamedFloatSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot")
	FName Name = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot")
	float Value = 0.0f;
};

/** 用名称标识的整数，避免把计数器强行转换为浮点数。 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponNamedIntSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot")
	FName Name = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot")
	int32 Value = 0;
};

/** 用名称标识的字符串，适合不能表示为数值的扩展状态。 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponNamedStringSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot")
	FName Name = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot")
	FString Value;
};

/**
 * 单个 GAS 属性的完整数值。
 * 同时保存 BaseValue 和 CurrentValue，避免只恢复表格基础值后丢失运行时修改；Damage 等瞬时 Meta Attribute 不应写入。
 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponGameplayAttributeSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Attribute")
	FGameplayTag AttributeTag;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Attribute")
	float BaseValue = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Attribute")
	float CurrentValue = 0.0f;
};

/** 动态属性修改器中的单条修改定义。 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponAttributeModifierEntrySnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Modifier")
	FGameplayTag AttributeTag;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Modifier")
	TEnumAsByte<EGameplayModOp::Type> ModifierOp = EGameplayModOp::Additive;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Modifier")
	float Magnitude = 0.0f;
};

/**
 * 一个天赋、模组或其他来源施加的整组动态修改器。
 * UAttributeModifierManager 创建的 GameplayEffect 位于 TransientPackage，不能依赖 EffectClass 路径恢复，因此必须保存原始定义。
 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponAttributeModifierGroupSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Modifier")
	FName SourceId = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Modifier")
	TArray<FWeaponAttributeModifierEntrySnapshot> Modifiers;
};

/**
 * 可持续 GameplayEffect 的恢复配方。
 * 不保存 FActiveGameplayEffectHandle、FGameplayEffectSpecHandle 或 UObject 指针；恢复时必须在新 ASC 上重新构建 Spec。
 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponGameplayEffectSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|GameplayEffect")
	FSoftClassPath EffectClass;

	/** 效果来源的稳定实体 ID；无效 ID 表示由系统施加或来源不可恢复。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|GameplayEffect")
	FWeaponStableEntityId SourceEntityId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|GameplayEffect")
	float Level = 1.0f;

	/** 负值表示无限持续时间。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|GameplayEffect")
	float RemainingDuration = -1.0f;

	/** 0 表示非周期效果。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|GameplayEffect")
	float Period = 0.0f;

	/** 距离下一次周期执行的剩余时间；负值表示没有待执行的周期。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|GameplayEffect")
	float TimeUntilNextPeriodicExecute = -1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|GameplayEffect", meta = (ClampMin = "1"))
	int32 StackCount = 1;

	/** 保存以 GameplayTag 为键的 SetByCaller 数值。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|GameplayEffect")
	TArray<FWeaponTaggedFloatSnapshot> SetByCallerMagnitudes;

	/** 保存以 FName 为键的 SetByCaller 数值。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|GameplayEffect")
	TArray<FWeaponNamedFloatSnapshot> SetByCallerNameMagnitudes;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|GameplayEffect")
	FGameplayTagContainer DynamicGrantedTags;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|GameplayEffect")
	FGameplayTagContainer DynamicAssetTags;
};

/** 单个玩家的完整权威状态。 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponPlayerSnapshot
{
	GENERATED_BODY()

	/** 平台或账号 ID 转换后的稳定字符串；不能使用 Controller、Pawn 或 PlayerState 指针代替。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	FWeaponStableEntityId StablePlayerId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	FString PlayerName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	int32 SlotIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	bool bIsReady = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	float Score = 0.0f;

	/** 恢复时要生成的 Pawn 类型。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	FSoftClassPath PawnClass;

	/** AWeaponCharacter 初始化静态基础属性时使用的 DataTable 行名。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	FName CharacterConfigRowName = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	FVector Location = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	FRotator Rotation = FRotator::ZeroRotator;

	/** 服务器确认的水平瞄准方向；恢复后射击和近战从此值继续。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	FVector AimDirection = FVector(1.0f, 0.0f, 0.0f);

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	FVector Velocity = FVector::ZeroVector;

	/** 对应 EMovementMode 的底层值，避免在纯数据头中依赖 CharacterMovementComponent。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	uint8 MovementMode = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	uint8 CustomMovementMode = 0;

	/** 覆盖全部 Health、Combat、Movement、Resistance 和 Resource 属性，是 GAS 数值的唯一快照来源。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	TArray<FWeaponGameplayAttributeSnapshot> Attributes;

	/** UWeaponDataAsset 的 PrimaryAssetId；静态武器定义由 AssetManager 重建，不复制进快照。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	TArray<FPrimaryAssetId> WeaponInventory;

	/** 当前武器在 WeaponInventory 中的索引，是恢复装备选择的唯一权威值。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	int32 CurrentWeaponIndex = INDEX_NONE;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	TArray<FWeaponGameplayEffectSnapshot> PersistentGameplayEffects;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	TArray<FWeaponAttributeModifierGroupSnapshot> ActiveAttributeModifiers;

	/** 只保存不由 GameplayEffect 派生的持久 Loose Tags，例如 State.Dead。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	FGameplayTagContainer LooseGameplayTags;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	bool bIsDead = false;

	/** 负值表示当前没有待触发的重生计时器。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	float RemainingRespawnTime = -1.0f;

	/** UAttributeRegenComponent 是否仍处于受击后的护盾恢复等待阶段。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	bool bShieldRegenDelayed = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Player")
	float ShieldRegenDelayRemaining = 0.0f;
};

/** 单个存活敌人的权威状态；已死亡并完成销毁的敌人通过不出现在数组中表示。 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponEnemySnapshot
{
	GENERATED_BODY()

	/** 由服务端敌人注册表分配，销毁和跨 World 恢复期间保持不变。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Enemy")
	FWeaponStableEntityId StableEnemyId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Enemy")
	FSoftClassPath EnemyClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Enemy")
	FName EnemyConfigRowName = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Enemy")
	FVector Location = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Enemy")
	FRotator Rotation = FRotator::ZeroRotator;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Enemy")
	FVector Velocity = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Enemy")
	uint8 MovementMode = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Enemy")
	uint8 CustomMovementMode = 0;

	/** 敌人的全部 GAS 数值，是生命、护盾及其他属性的唯一快照来源。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Enemy")
	TArray<FWeaponGameplayAttributeSnapshot> Attributes;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Enemy")
	TArray<FWeaponGameplayEffectSnapshot> PersistentGameplayEffects;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Enemy")
	TArray<FWeaponAttributeModifierGroupSnapshot> ActiveAttributeModifiers;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Enemy")
	FGameplayTagContainer LooseGameplayTags;

	/** 生成该敌人的波次；当前 Weapon Variant 尚未提供可捕获的波次注册表。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Enemy")
	int32 SpawnWave = INDEX_NONE;

	/** BTTask_Attack 节点内冷却的剩余秒数；负值表示当前不在攻击冷却任务中。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Enemy")
	float AttackCooldownRemaining = -1.0f;

	/** 当前 AI 目标的稳定 ID；为空时由恢复后的感知系统重新选择目标。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Enemy")
	FWeaponStableEntityId PerceivedTargetId;
};

/** 一枚仍在服务器世界中飞行的权威投射物。 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponProjectileSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Projectile")
	FWeaponStableEntityId StableProjectileId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Projectile")
	FSoftClassPath ProjectileClass;

	/** 生成该投射物的 WeaponData/GrenadeData 等静态配置资产。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Projectile")
	FPrimaryAssetId SourceConfigAssetId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Projectile")
	FTransform Transform = FTransform::Identity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Projectile")
	FVector Velocity = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Projectile")
	float RemainingLifeSeconds = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Projectile")
	FSoftClassPath DamageEffectClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Projectile")
	float DamageValue = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Projectile")
	FWeaponStableEntityId InstigatorId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Projectile")
	FGameplayTagContainer DamageTags;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Projectile")
	bool bPenetratesTerrain = false;
};

/** 仍在服务器世界中等待引信的手雷。静态爆炸参数由 GrenadeDataAsset 重建。 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponGrenadeProjectileSnapshot
{
	GENERATED_BODY()

	/** 所有投射物共享的身份、移动、伤害和来源状态。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Grenade")
	FWeaponProjectileSnapshot Projectile;

	/** 距离服务端引信触发的剩余秒数。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Grenade")
	float RemainingFuseSeconds = 0.0f;

	/** 正常情况下活动手雷恒为 false；保留该值用于拒绝恢复已引爆对象。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Grenade")
	bool bHasExploded = false;
};

/** 单个服务端任务目标的进度。 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponObjectiveSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Objective")
	FName ObjectiveId = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Objective")
	int32 CurrentProgress = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Objective")
	int32 TargetProgress = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Objective")
	bool bCompleted = false;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Objective")
	FGameplayTag ObjectiveState;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Objective")
	TArray<FWeaponNamedIntSnapshot> MutableIntegers;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Objective")
	TArray<FWeaponTaggedFloatSnapshot> MutableValues;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Objective")
	TArray<FWeaponNamedStringSnapshot> MutableText;
};

/** 恢复世界时需要重新生成的掉落物或拾取物。 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponWorldDropSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Drop")
	FWeaponStableEntityId StableDropId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Drop")
	FPrimaryAssetId ItemId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Drop")
	FSoftClassPath ActorClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Drop")
	FTransform Transform = FTransform::Identity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Drop", meta = (ClampMin = "1"))
	int32 Quantity = 1;

	/** 负值表示由对象自身规则决定生命周期。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Drop")
	float RemainingLifeSeconds = -1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Drop")
	TArray<FWeaponTaggedFloatSnapshot> MutableValues;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Drop")
	TArray<FWeaponNamedStringSnapshot> MutableText;
};

/**
 * 尚未拥有专用 DTO 的权威 Actor 的通用后备格式。
 * 一旦某类 Actor 出现复杂引用或生命周期规则，应升级为独立强类型快照，不能长期依赖此结构。
 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponMutableActorSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	FWeaponStableEntityId StableActorId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	FSoftClassPath ActorClass;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	FTransform Transform = FTransform::Identity;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	FVector Velocity = FVector::ZeroVector;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	bool bIsActive = true;

	/** 负值表示由对象自身规则决定生命周期。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	float RemainingLifeSeconds = -1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	TArray<FPrimaryAssetId> ReferencedAssets;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	TArray<FWeaponNamedIntSnapshot> MutableIntegers;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	TArray<FWeaponTaggedFloatSnapshot> MutableValues;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	TArray<FWeaponNamedStringSnapshot> MutableText;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	FGameplayTagContainer StateTags;
};

/**
 * 从确定性基础 GridMap 生成结果上覆盖的单格差异。
 * 只记录类型或耐久发生变化的格子，不复制整张地图。
 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponGridTileDeltaSnapshot
{
	GENERATED_BODY()

	/** 对应运行时基础地图世代；恢复时不得跨世代应用。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Map")
	int32 GenerationVersion = 0;

	/** 同一地图世代内的服务器提交版本。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Map")
	int64 Revision = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Map")
	int32 GridX = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Map")
	int32 GridY = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Map")
	EGridTileType TileType = EGridTileType::Empty;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Map")
	int32 Health = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Map")
	int32 MaxHealth = 0;
};

/** 一张 GridMap 的确定性生成基线和运行时差异。 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponGridMapSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Map")
	FWeaponStableEntityId StableMapId;

	/** 与运行时 FGridMapBaselineState::SchemaVersion 对应。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Map")
	int32 BaselineSchemaVersion = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Map")
	int32 GenerationVersion = 0;

	/** 与运行时 BaselineState.Config 保持同一字段布局。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Map")
	FGridMapConfig BaselineConfig;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Map")
	FSoftClassPath GeneratorClass;

	/** 服务器实际使用并复制给客户端的非零种子。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Map")
	int32 EffectiveSeed = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Map")
	bool bGenerated = false;

	/** 恢复后新的 Tile Delta 必须从该版本继续递增。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Map")
	int64 NextTileRevision = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Map")
	TArray<FWeaponGridTileDeltaSnapshot> TileDeltas;
};

/** 当前战局中所有可恢复的服务端世界状态。 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponWorldSnapshot
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	int32 CurrentWave = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	FName CurrentWaveId = NAME_None;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	TArray<FWeaponEnemySnapshot> LivingEnemies;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	TArray<FWeaponProjectileSnapshot> ActiveProjectiles;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	TArray<FWeaponGrenadeProjectileSnapshot> ActiveGrenades;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	TArray<FWeaponObjectiveSnapshot> Objectives;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	TArray<FWeaponWorldDropSnapshot> Drops;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	TArray<FWeaponMutableActorSnapshot> OtherAuthoritativeState;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	TArray<FWeaponGridMapSnapshot> GridMaps;

	/** 不归属于某个 Actor 的全局权威计数器。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	TArray<FWeaponNamedIntSnapshot> GlobalIntegers;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	TArray<FWeaponNamedFloatSnapshot> GlobalValues;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	TArray<FWeaponNamedStringSnapshot> GlobalText;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|World")
	FGameplayTagContainer GlobalStateTags;
};

/** 可保存、传输并在新服务器上恢复的版本化战局根快照。 */
USTRUCT(BlueprintType)
struct LETSDEEPROCK_API FWeaponMatchSnapshot
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, SaveGame, Category = "Snapshot|Match")
	int32 SchemaVersion = WeaponMatchSnapshotVersion;

	/** 每次捕获生成的新 GUID，用于去重同一版本的传输包。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Match")
	FGuid SnapshotId;

	/** 同一 MatchId 内单调递增，用于选出最新合法快照。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Match")
	int64 SnapshotRevision = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Match")
	FGuid MatchId;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Match")
	FString ServerDisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Match")
	int32 MaxPlayers = 4;

	/** 恢复后应使用的 GameMode 类型。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Match")
	FSoftClassPath GameModeClass;

	/** 地图长包名，例如 /Game/Variant_Weapon/Maps/NewMap。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Match")
	FString MapPackageName;

	/** 战局级初始随机种子；GridMap 的实际种子保存在各自的 FWeaponGridMapSnapshot 中。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Match")
	int32 RandomSeed = 0;

	/** 若战局使用持久 FRandomStream，则保存捕获时的 CurrentSeed 以继续随机序列。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Match")
	int32 RandomStreamCurrentSeed = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Match")
	EWeaponMatchPhase MatchPhase = EWeaponMatchPhase::Waiting;

	/** 捕获时 GameState 的同步服务器时间，仅用于解释同一 World 内的绝对时间。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Match")
	double ServerWorldTimeSeconds = 0.0;

	/** 捕获时阶段计时器的剩余秒数；新服务器应基于自己的时间轴重建截止时间。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Match")
	float RemainingPhaseTime = 0.0f;

	/** 仅用于诊断和快照新旧比较；Gameplay 恢复不得依赖不同机器的本地墙钟。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Match")
	FDateTime CapturedAtUtc;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Match")
	TArray<FWeaponPlayerSnapshot> Players;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, Category = "Snapshot|Match")
	FWeaponWorldSnapshot World;
};
