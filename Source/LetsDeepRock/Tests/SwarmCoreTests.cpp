// Swarm 核心类型、原型调参与伤害数学自动化测试

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GameplayTagContainer.h"
#include "Data/DataTableRows.h"
#include "WeaponGameplayTags.h"
#include "Swarm/SwarmTypes.h"

#define SWARM_TEST(ClassName, Path) IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, Path, EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

SWARM_TEST(FSwarmTuningLookupTest, "LetsDeepRock.Swarm.Types.TuningLookup")
bool FSwarmTuningLookupTest::RunTest(const FString& Parameters)
{
	const FSwarmArchetypeTuning* Grunt = SwarmFindArchetypeTuning(TEXT("BasicGrunt"));
	const FSwarmArchetypeTuning* Runner = SwarmFindArchetypeTuning(TEXT("FastRunner"));
	const FSwarmArchetypeTuning* Exploder = SwarmFindArchetypeTuning(TEXT("Exploder"));
	TestNotNull(TEXT("BasicGrunt"), Grunt);
	TestNotNull(TEXT("FastRunner"), Runner);
	TestNotNull(TEXT("Exploder"), Exploder);
	TestNull(TEXT("HeavyTank"), SwarmFindArchetypeTuning(TEXT("HeavyTank")));
	TestNull(TEXT("Spitter"), SwarmFindArchetypeTuning(TEXT("Spitter")));
	TestNull(TEXT("None"), SwarmFindArchetypeTuning(NAME_None));
	if (Exploder) { TestEqual(TEXT("Exploder ExplodeRadius"), Exploder->ExplodeRadius, 250.f); }
	if (Grunt) { TestEqual(TEXT("BasicGrunt AttackInterval"), Grunt->AttackInterval, 1.f); }
	if (Runner) { TestEqual(TEXT("FastRunner CollisionRadius"), Runner->CollisionRadius, 22.f); }
	return true;
}

SWARM_TEST(FSwarmDamageMathTest, "LetsDeepRock.Swarm.Types.DamageMath")
bool FSwarmDamageMathTest::RunTest(const FString& Parameters)
{
	FEnemyConfigRow Row;
	Row.ResistPhysical = 0.2f;
	TestEqual(TEXT("Physical"), SwarmComputeDamage(10.f, FGameplayTagContainer(TAG_Damage_Type_Physical), Row), 8.f);
	TestEqual(TEXT("无类型标签视为物理"), SwarmComputeDamage(10.f, FGameplayTagContainer(), Row), 8.f);

	Row.ResistFire = 0.5f;
	TestEqual(TEXT("Fire"), SwarmComputeDamage(10.f, FGameplayTagContainer(TAG_Damage_Type_Fire), Row), 5.f);
	TestEqual(TEXT("True 无视抗性"), SwarmComputeDamage(10.f, FGameplayTagContainer(TAG_Damage_Type_True), Row), 10.f);

	Row.ResistCold = 1.5f;
	TestEqual(TEXT("抗性过高钳制为 0"), SwarmComputeDamage(10.f, FGameplayTagContainer(TAG_Damage_Type_Cold), Row), 0.f);
	return true;
}

SWARM_TEST(FSwarmStatusResolveTest, "LetsDeepRock.Swarm.Types.StatusResolve")
bool FSwarmStatusResolveTest::RunTest(const FString& Parameters)
{
	const FGameplayTagContainer Empty;
	TestEqual(TEXT("Fire tag"), SwarmResolveStatus(FGameplayTagContainer(TAG_Damage_Type_Fire), NAME_None), ESwarmStatusFlags::Burning);
	TestEqual(TEXT("Burning id"), SwarmResolveStatus(Empty, TEXT("Burning")), ESwarmStatusFlags::Burning);
	TestEqual(TEXT("Cold tag"), SwarmResolveStatus(FGameplayTagContainer(TAG_Damage_Type_Cold), NAME_None), ESwarmStatusFlags::Frozen);
	TestEqual(TEXT("Frozen id"), SwarmResolveStatus(Empty, TEXT("Frozen")), ESwarmStatusFlags::Frozen);
	TestEqual(TEXT("Shocked"), SwarmResolveStatus(Empty, TEXT("Shocked")), ESwarmStatusFlags::None);
	TestEqual(TEXT("Corroded"), SwarmResolveStatus(Empty, TEXT("Corroded")), ESwarmStatusFlags::None);
	TestEqual(TEXT("Electric tag"), SwarmResolveStatus(FGameplayTagContainer(TAG_Damage_Type_Electric), NAME_None), ESwarmStatusFlags::None);
	TestTrue(TEXT("Fire tag + Frozen id"),
		SwarmResolveStatus(FGameplayTagContainer(TAG_Damage_Type_Fire), TEXT("Frozen")) == (ESwarmStatusFlags::Burning | ESwarmStatusFlags::Frozen));
	return true;
}

#undef SWARM_TEST

#endif // WITH_DEV_AUTOMATION_TESTS
