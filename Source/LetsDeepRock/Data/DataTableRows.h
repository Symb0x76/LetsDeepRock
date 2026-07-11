// DataTable 行结构体定义
// 所有配置表的 C++ 行结构在此集中声明

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataTable.h"
#include "GameplayTagContainer.h"
#include "DataTableRows.generated.h"

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
