// Swarm 池化虫群：核心类型、原型调参与伤害数学

#include "Swarm/SwarmTypes.h"
#include "Data/DataTableRows.h"
#include "WeaponGameplayTags.h"

const FSwarmArchetypeTuning* SwarmFindArchetypeTuning(const FName ArchetypeRow)
{
	// 字段顺序：MeshScale, CollisionRadius, AttackRange, AttackInterval, ExplodeRadius
	static const FSwarmArchetypeTuning SwarmBasicGrunt{0.45f, 28.f, 60.f, 1.0f, 0.f};
	static const FSwarmArchetypeTuning SwarmFastRunner{0.35f, 22.f, 50.f, 0.8f, 0.f};
	static const FSwarmArchetypeTuning SwarmExploder{0.60f, 36.f, 40.f, 0.0f, 250.f};

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
