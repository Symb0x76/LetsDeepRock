// Swarm 池化虫群：核心类型、原型调参与伤害数学

#include "Swarm/SwarmTypes.h"
#include "Data/DataTableRows.h"
#include "WeaponGameplayTags.h"

const FSwarmArchetypeTuning* SwarmFindArchetypeTuning(const FName ArchetypeRow)
{
	// 字段顺序：MeshScale, CollisionRadius, AttackRange, AttackInterval, ExplodeRadius
	// 体型：BasicGrunt(Scavenger) 体长 = 玩家胶囊直径 68uu；其余按原始 GLB 体长等比例
	// (Scavenger 2.351m / Hunter 3.598m / Warrior 4.414m)。静态网格已归一化为 100uu 长，
	// MeshScale = 目标体长 / 100，CollisionRadius = 目标体长 / 2。与 BP_Enemy_* 保持一致。
	static const FSwarmArchetypeTuning SwarmBasicGrunt{0.680f, 34.0f, 60.f, 1.0f, 0.f};
	static const FSwarmArchetypeTuning SwarmFastRunner{1.041f, 52.0f, 50.f, 0.8f, 0.f};
	static const FSwarmArchetypeTuning SwarmExploder{1.277f, 63.8f, 40.f, 0.0f, 250.f};

	if (ArchetypeRow == TEXT("BasicGrunt")) { return &SwarmBasicGrunt; }
	if (ArchetypeRow == TEXT("FastRunner")) { return &SwarmFastRunner; }
	if (ArchetypeRow == TEXT("Exploder")) { return &SwarmExploder; }
	return nullptr;
}

float SwarmComputeDamage(const float RawDamage, const FGameplayTagContainer& DamageTags, const FEnemyConfigRow& Row)
{
	// 与 DamageExecCalculation 同一优先级：True > Fire > Cold > Poison > Explosion > Electric > 物理
	if (DamageTags.HasTag(TAG_Damage_Type_True)) { return FMath::Max(0.f, RawDamage); }

	float Resist = Row.ResistPhysical;
	if (DamageTags.HasTag(TAG_Damage_Type_Fire)) { Resist = Row.ResistFire; }
	else if (DamageTags.HasTag(TAG_Damage_Type_Cold)) { Resist = Row.ResistCold; }
	else if (DamageTags.HasTag(TAG_Damage_Type_Poison)) { Resist = Row.ResistPoison; }
	else if (DamageTags.HasTag(TAG_Damage_Type_Explosion)) { Resist = Row.ResistExplosion; }
	else if (DamageTags.HasTag(TAG_Damage_Type_Electric)) { Resist = Row.ResistElectric; }
	return FMath::Max(0.f, RawDamage * (1.f - Resist));
}

ESwarmStatusFlags SwarmResolveStatus(const FGameplayTagContainer& DamageTags, const FName StatusEffectId)
{
	ESwarmStatusFlags Flags = ESwarmStatusFlags::None;
	if (DamageTags.HasTag(TAG_Damage_Type_Fire) || StatusEffectId == TEXT("Burning")) { Flags |= ESwarmStatusFlags::Burning; }
	if (DamageTags.HasTag(TAG_Damage_Type_Cold) || StatusEffectId == TEXT("Frozen")) { Flags |= ESwarmStatusFlags::Frozen; }
	return Flags;
}
