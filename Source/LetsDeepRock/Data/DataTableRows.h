// DataTable 行结构体定义
// 所有配置表的 C++ 行结构在此集中声明

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "DataTableRows.generated.h"

/**
 * 职业配置表行
 * 对应 DataTable: DT_ProfessionConfig
 */
USTRUCT(BlueprintType)
struct FProfessionConfigRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Profession")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Profession")
	FName CharacterAttributeRow;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Loadout")
	FName PrimaryWeaponId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Loadout")
	FName SecondaryWeaponId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Loadout")
	FName SkillItemId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Loadout")
	FName ClassEquipmentId;

	/** 可供玩家选择的手雷配置行，英文逗号分隔 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Loadout")
	FString GrenadeIds;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Loadout")
	FName GrenadeVariantSetId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Loadout")
	FName DefaultGrenadeId;

	/** 可供玩家选择的护甲词条，英文逗号分隔，当前允许为空 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Armor")
	FString ArmorPerkIds;
};

/**
 * 职业武器池配置表行
 * 对应 DataTable: DT_ProfessionWeaponPool
 */
USTRUCT(BlueprintType)
struct FProfessionWeaponPoolRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ProfessionWeaponPool")
	FName ProfessionId;

	/** Primary 或 Secondary */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ProfessionWeaponPool")
	FName SlotType;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ProfessionWeaponPool")
	FName WeaponId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "ProfessionWeaponPool")
	bool bDefaultEquipped = false;
};

/**
 * 武器定义表行
 * 对应 DataTable: DT_WeaponConfig
 */
USTRUCT(BlueprintType)
struct FWeaponConfigRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	FText DisplayName;

	/** 可用射击模式配置行，英文逗号分隔 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	FString FireModeIds;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Weapon")
	FName DefaultFireModeId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ammo")
	FName AmmoType = TEXT("None");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ammo", meta = (ClampMin = 0))
	int32 MagazineSize = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ammo", meta = (ClampMin = 0))
	int32 ReserveAmmo = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Ammo", meta = (ClampMin = 0))
	float ReloadDuration = 0.0f;

	/** StaticMesh 资产路径 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual")
	FString WeaponMesh;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual")
	FName MuzzleSocketName = TEXT("Muzzle");
};

/**
 * 武器射击模式定义表行
 * 对应 DataTable: DT_WeaponFireModeConfig
 */
USTRUCT(BlueprintType)
struct FWeaponFireModeConfigRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "FireMode")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "FireMode")
	FName TriggerType = TEXT("Press");

	/** GameplayAbility 类路径 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "FireMode")
	FString FireAbilityClass;

	/** Projectile Actor 类路径 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile")
	FString ProjectileClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Damage")
	FGameplayTag DamageTypeTag;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Damage", meta = (ClampMin = 0))
	float BaseDamage = 10.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Damage", meta = (ClampMin = 0))
	float ArmorPenetration = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Damage", meta = (ClampMin = 0))
	float WeakpointMultiplier = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timing", meta = (ClampMin = 0))
	float FireInterval = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Timing", meta = (ClampMin = 0))
	float ChargeDuration = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile", meta = (ClampMin = 0))
	float ProjectileSpeed = 3000.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile", meta = (ClampMin = 0))
	float ProjectileLifetime = 0.0f;

	/** 后续状态效果机制使用的配置行 ID；阶段1仅保证可读取 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Effect")
	FName StatusEffectId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile", meta = (ClampMin = 0))
	float ProjectileGravityScale = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile", meta = (ClampMin = 1))
	int32 MaxHitTargets = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Projectile", meta = (ClampMin = 1))
	int32 ProjectileCount = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Area", meta = (ClampMin = 0))
	float AreaDamage = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Area", meta = (ClampMin = 0))
	float AreaRadius = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Area", meta = (ClampMin = 0))
	float AreaFalloff = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement", meta = (ClampMin = 0))
	float MoveSpeedMultiplier = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement")
	bool bCanFireWhileMoving = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Accuracy", meta = (ClampMin = 0))
	float SpreadAngle = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Accuracy", meta = (ClampMin = 0))
	float Recoil = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Range", meta = (ClampMin = 0))
	float DamageFalloffStart = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Range", meta = (ClampMin = 0))
	float DamageFalloffEnd = 0.0f;
};

/**
 * 手雷定义表行
 * 对应 DataTable: DT_GrenadeConfig
 */
USTRUCT(BlueprintType)
struct FGrenadeConfigRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grenade")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grenade")
	FName VariantSetId;

	/** GameplayAbility 类路径 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grenade")
	FString ThrowAbilityClass;

	/** Grenade Actor 类路径 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grenade")
	FString GrenadeActorClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Inventory", meta = (ClampMin = 0))
	int32 MaxCarryCount = 4;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Inventory", meta = (ClampMin = 0))
	int32 ResupplyCount = 2;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Throw", meta = (ClampMin = 0))
	float MaxThrowDistance = 1600.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Damage", meta = (ClampMin = 0))
	float AreaRadius = 250.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Damage", meta = (ClampMin = 0))
	float BaseDamage = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Damage")
	FGameplayTag DamageTypeTag;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Effect")
	FName StatusEffectId;
};

/**
 * 异常状态效果定义表行
 * 对应 DataTable: DT_StatusEffectConfig
 */
USTRUCT(BlueprintType)
struct FStatusEffectConfigRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StatusEffect")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StatusEffect")
	FGameplayTag StatusTag;

	/** GameplayEffect 类路径 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StatusEffect")
	FString GameplayEffectClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StatusEffect", meta = (ClampMin = 0))
	float Duration = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StatusEffect", meta = (ClampMin = 0))
	float TickInterval = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StatusEffect", meta = (ClampMin = 0))
	float Magnitude = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StatusEffect")
	float DamagePerTick = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StatusEffect")
	float MoveSpeedMultiplier = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StatusEffect")
	float AttackSpeedMultiplier = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StatusEffect")
	float ArmorModifier = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StatusEffect", meta = (ClampMin = 1))
	int32 MaxStacks = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StatusEffect", meta = (ClampMin = 0, ClampMax = 1))
	float ApplyChance = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StatusEffect", meta = (ClampMin = 1))
	int32 HitsPerStack = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "StatusEffect")
	FName RefreshPolicy;
};

/**
 * 职业工具定义表行
 * 对应 DataTable: DT_ToolConfig
 */
USTRUCT(BlueprintType)
struct FToolConfigRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tool")
	FText DisplayName;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tool")
	FName ToolType;

	/** GameplayAbility 类路径 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tool")
	FString ActivationAbilityClass;

	/** Actor 类路径 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tool")
	FString SpawnedActorClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tool", meta = (ClampMin = 0))
	int32 MaxUseCount = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tool", meta = (ClampMin = 0))
	int32 ResupplyAmount = 0;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tool", meta = (ClampMin = 0))
	float Cooldown = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tool", meta = (ClampMin = 0))
	float SpawnDistance = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Tool")
	FName BehaviorConfigId;
};

/**
 * 无人机行为配置表行
 * 对应 DataTable: DT_DroneConfig
 */
USTRUCT(BlueprintType)
struct FDroneConfigRow : public FTableRowBase
{
	GENERATED_BODY()

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lifetime", meta = (ClampMin = 0))
	float Duration = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lifetime", meta = (ClampMin = 0))
	float Cooldown = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Lifetime", meta = (ClampMin = 1))
	int32 MaxActiveCount = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deploy", meta = (ClampMin = 0))
	float SpawnDistance = 0.0f;

	/** Actor 类路径 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Deploy")
	FString SpawnedActorClass;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement", meta = (ClampMin = 0))
	float HoverHeight = 150.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement", meta = (ClampMin = 0))
	float OrbitRadius = 300.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement")
	float OrbitSpeed = 45.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Vision", meta = (ClampMin = 0))
	float VisionRadius = 800.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Vision", meta = (ClampMin = 0))
	float DetectionInterval = 0.2f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Vision")
	bool bRequireLineOfSight = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mark")
	FName EnemyMarkId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mark")
	FName ResourceMarkId;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mark", meta = (ClampMin = 0))
	float MarkDurationAfterLeaving = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Mark", meta = (ClampMin = 0))
	int32 MaxMarkedTargets = 0;

	/** Niagara 资产路径 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual")
	FString VisualEffect;

	/** 颜色字符串，格式为 R,G,B,A */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual")
	FString LightColor = TEXT("1,1,1,1");

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual", meta = (ClampMin = 0))
	float LightIntensity = 5000.0f;

	/** Sound 资产路径 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual")
	FString LoopSound;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Detection")
	bool bDetectEnemies = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Detection")
	bool bDetectResources = true;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Detection")
	bool bShareMarksWithTeam = true;
};

/**
 * 敌人配置表行
 * 对应 DataTable: DT_EnemyConfig
 * 用于定义不同种类敌人的基础属性
 */
USTRUCT(BlueprintType)
struct FEnemyConfigRow : public FTableRowBase
{
	GENERATED_BODY()

	/** 敌人显示名称 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Basic")
	FText DisplayName;

	/** 最大生命值 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attributes")
	float MaxHealth = 30.0f;

	/** 移动速度 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attributes")
	float MoveSpeed = 400.0f;

	/** 攻击伤害 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
	float AttackDamage = 10.0f;

	/** 击杀奖励分数 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Reward")
	int32 ScoreValue = 1;

	/** 死亡掉落拾取物概率（0-100） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Reward", meta = (ClampMin = 0, ClampMax = 100))
	int32 PickupSpawnChance = 10;

	/** 死亡后延迟销毁时间（秒） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Visual", meta = (ClampMin = 0))
	float DeathLingerTime = 2.0f;

	/** 护甲值（参与减伤公式） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attributes", meta = (ClampMin = 0))
	float Armor = 0.0f;

	/** 最大护盾值 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attributes", meta = (ClampMin = 0))
	float MaxShield = 0.0f;

	/** 物理抗性 (0~1) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resistance", meta = (ClampMin = 0, ClampMax = 1))
	float ResistPhysical = 0.0f;

	/** 火焰抗性 (0~1) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resistance", meta = (ClampMin = 0, ClampMax = 1))
	float ResistFire = 0.0f;

	/** 冰霜抗性 (0~1) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resistance", meta = (ClampMin = 0, ClampMax = 1))
	float ResistCold = 0.0f;

	/** 毒素抗性 (0~1) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resistance", meta = (ClampMin = 0, ClampMax = 1))
	float ResistPoison = 0.0f;

	/** 爆炸抗性 (0~1) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resistance", meta = (ClampMin = 0, ClampMax = 1))
	float ResistExplosion = 0.0f;

	/** 电击抗性 (0~1) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resistance", meta = (ClampMin = 0, ClampMax = 1))
	float ResistElectric = 0.0f;

	/** 敌人护甲类型 Tag（如 "ArmorType.Light"） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Attributes")
	FGameplayTag ArmorTypeTag;
};

/**
 * 生成器/波次配置表行
 * 对应 DataTable: DT_SpawnerConfig
 * 用于定义生成器的行为参数
 */
USTRUCT(BlueprintType)
struct FSpawnerConfigRow : public FTableRowBase
{
	GENERATED_BODY()

	/** 生成组间隔（秒） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawn", meta = (ClampMin = 0))
	float SpawnGroupDelay = 5.0f;

	/** 每组生成数量 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawn", meta = (ClampMin = 1))
	int32 SpawnGroupSize = 3;

	/** 个体生成最小间隔（秒） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawn", meta = (ClampMin = 0))
	float MinSpawnDelay = 0.33f;

	/** 个体生成最大间隔（秒） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawn", meta = (ClampMin = 0))
	float MaxSpawnDelay = 0.66f;

	/** 生成半径（cm） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawn", meta = (ClampMin = 0))
	float SpawnRadius = 600.0f;

	/** 场上 NPC 数量上限 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawn", meta = (ClampMin = 1))
	int32 NPCCap = 20;

	/** 每房虫群配额(只数) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Swarm", meta = (ClampMin = 0))
	int32 SwarmQuotaPerRoom = 60;

	/** 每次爆发生成的虫群数量 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Swarm", meta = (ClampMin = 1))
	int32 SwarmBurstSize = 8;

	/** 场上虫群存活数上限 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Swarm", meta = (ClampMin = 1))
	int32 SwarmCap = 120;

	/** 每房精英配额(只数,走 NPCCap 限制的常规刷怪) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Swarm", meta = (ClampMin = 0))
	int32 EliteQuotaPerRoom = 2;
};

/**
 * 游戏模式全局配置表行
 * 对应 DataTable: DT_GameModeConfig
 * 用于定义连击系统、难度等全局平衡参数
 */
USTRUCT(BlueprintType)
struct FGameModeConfigRow : public FTableRowBase
{
	GENERATED_BODY()

	/** 连击递增阈值（击杀多少次后连击倍率+1） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = 1))
	int32 ComboIncrementMax = 5;

	/** 最大连击倍率 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = 1))
	int32 ComboCap = 4;

	/** 连击冷却时间（秒，超时则连击衰减） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combo", meta = (ClampMin = 0))
	float ComboCooldown = 3.0f;

	/** 场上 NPC 数量上限 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Spawn", meta = (ClampMin = 1))
	int32 NPCCap = 20;
};

/**
 * 角色/职业属性基础值表行
 * 对应 DataTable: DT_CharacterAttributes
 * 用于定义不同职业的初始属性
 */
USTRUCT(BlueprintType)
struct FCharacterAttributeRow : public FTableRowBase
{
	GENERATED_BODY()

	// --- 生存 ---
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Health")
	float MaxHealth = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Health")
	float MaxShield = 50.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Health")
	float ShieldRegenRate = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Health")
	float ShieldRegenDelay = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Health")
	float Armor = 0.0f;

	// --- 战斗 ---
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
	float CritChance = 0.05f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
	float CritDamage = 1.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
	float ArmorPenetration = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
	float DamageBonus = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
	float WeakpointBonus = 0.5f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
	float AttackSpeed = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Combat")
	float AreaDamageBonus = 0.0f;

	// --- 移动 ---
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement")
	float MoveSpeed = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement")
	float SprintSpeed = 1.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Movement")
	float DodgeDistance = 600.0f;

	// --- 视野 ---
	/** 以角色朝向为中心，向左右各扩展的角度 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Vision", meta = (ClampMin = 0, ClampMax = 180))
	float VisionHalfAngle = 60.0f;

	/** 最大视野距离（cm），0 表示不限制距离 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Vision", meta = (ClampMin = 0))
	float VisionRange = 0.0f;

	// --- 抗性 ---
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resistance", meta = (ClampMin = 0, ClampMax = 1))
	float ResistPhysical = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resistance", meta = (ClampMin = 0, ClampMax = 1))
	float ResistFire = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resistance", meta = (ClampMin = 0, ClampMax = 1))
	float ResistCold = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resistance", meta = (ClampMin = 0, ClampMax = 1))
	float ResistPoison = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resistance", meta = (ClampMin = 0, ClampMax = 1))
	float ResistExplosion = 0.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resistance", meta = (ClampMin = 0, ClampMax = 1))
	float ResistElectric = 0.0f;

	// --- 资源 ---
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resource")
	float MaxEnergy = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resource")
	float EnergyRegenRate = 5.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resource")
	float MaxStamina = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resource")
	float StaminaRegenRate = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Resource")
	float Lucky = 0.0f;
};

/**
 * 伤害类型修正表行
 *
 * 对应 DataTable: DT_DamageTypeModifier
 * 行名使用伤害类型 Tag 字符串（如 "Damage.Type.Fire"）
 * 每列为不同护甲类型的伤害修正系数（>1 克制，<1 抵抗）
 */
USTRUCT(BlueprintType)
struct FDamageTypeModifierRow : public FTableRowBase
{
	GENERATED_BODY()

	/** 对无护甲目标的修正系数 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Modifier")
	float VsUnarmored = 1.0f;

	/** 对轻甲目标的修正系数 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Modifier")
	float VsLight = 1.0f;

	/** 对重甲目标的修正系数 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Modifier")
	float VsHeavy = 1.0f;

	/** 对有机目标的修正系数 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Modifier")
	float VsOrganic = 1.0f;
};

/**
 * 天赋/模组效果表行
 * 对应 DataTable: DT_ModifierEffects
 *
 * 一个天赋/模组可有多行（修改多个属性），用相同 SourceId 关联。
 * 运行时由 UAttributeModifierManager 读取并动态创建 GE 应用。
 */
USTRUCT(BlueprintType)
struct FModifierEffectRow : public FTableRowBase
{
	GENERATED_BODY()

	/** 来源标识（天赋ID 或 模组ID），相同 SourceId 的行属于同一组 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Modifier")
	FName SourceId;

	/** 目标属性 Tag（如 "Attr.CritChance"） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Modifier")
	FGameplayTag AttributeTag;

	/** 运算类型：Additive=加算, Multiplicitive=乘算, Division=除算, Override=覆盖 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Modifier")
	TEnumAsByte<EGameplayModOp::Type> ModOp = EGameplayModOp::Additive;

	/** 修改数值 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Modifier")
	float Magnitude = 0.0f;
};
