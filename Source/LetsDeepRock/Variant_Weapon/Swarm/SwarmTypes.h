// Swarm 池化虫群：核心类型、原型调参与伤害数学

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"
#include "UObject/WeakObjectPtrTemplates.h"

struct FEnemyConfigRow;
class AActor;

/** 虫群常量（池容量、网络频率、状态参数） */
namespace SwarmConstants
{
	inline constexpr int32 PoolCapacity = 200;
	inline constexpr float NetUpdateHz = 12.f;
	inline constexpr float NetCullDistance = 5000.f;
	inline constexpr float TargetSearchRadius = 4000.f;
	inline constexpr float RetargetInterval = 0.5f;
	inline constexpr float SeparationRadius = 90.f;
	/** 碰撞柱半高：柱体立于地面、高 200，覆盖枪口弹道高度，避免子弹从矮小虫体上方飞过 */
	inline constexpr float CollisionHalfHeight = 100.f;
	inline constexpr float BurnDuration = 3.f;
	inline constexpr float BurnDps = 5.f;
	inline constexpr float FrozenDuration = 2.5f;
	inline constexpr float FrozenSpeedScale = 0.4f;
}

/** 虫群仅支持的轻量状态：燃烧、冰冻（减速） */
enum class ESwarmStatusFlags : uint8
{
	None = 0,
	Burning = 1 << 0,
	Frozen = 1 << 1,
};
ENUM_CLASS_FLAGS(ESwarmStatusFlags);

/** 一次对虫群个体的伤害事件 */
struct FSwarmDamageEvent
{
	float Damage = 0.f;
	FGameplayTagContainer DamageTags;
	FName StatusEffectId;
	TWeakObjectPtr<AActor> Instigator;
};

/** 单个虫的渲染/同步视图 */
struct FSwarmBugView
{
	FVector Location = FVector::ZeroVector;
	FName ArchetypeRow;
	float HealthFraction = 1.f;
};

/** 原型调参（生命、速度、伤害仍取自 DT_EnemyConfig 行） */
struct FSwarmArchetypeTuning
{
	float MeshScale = 1.f;
	float CollisionRadius = 30.f;
	float AttackRange = 60.f;
	float AttackInterval = 1.f;
	float ExplodeRadius = 0.f;
};

/** 按 DT_EnemyConfig 行名查找原型调参；非虫群行返回 nullptr */
const FSwarmArchetypeTuning* SwarmFindArchetypeTuning(FName ArchetypeRow);

/** 按伤害类型标签套用对应抗性；True 无视抗性，无类型标签视为物理；护甲忽略 */
float SwarmComputeDamage(float RawDamage, const FGameplayTagContainer& DamageTags, const FEnemyConfigRow& Row);

/** 由伤害标签与状态效果 ID 解析出虫群支持的状态；其余状态一律忽略 */
ESwarmStatusFlags SwarmResolveStatus(const FGameplayTagContainer& DamageTags, FName StatusEffectId);
