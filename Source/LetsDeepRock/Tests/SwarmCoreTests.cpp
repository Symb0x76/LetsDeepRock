// Swarm 核心类型、原型调参与伤害数学自动化测试

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GameplayTagContainer.h"
#include "Data/DataTableRows.h"
#include "WeaponGameplayTags.h"
#include "Swarm/SwarmTypes.h"
#include "Swarm/SwarmSpatialHash.h"
#include "Swarm/SwarmSteering.h"

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

SWARM_TEST(FSwarmSpatialHashTest, "LetsDeepRock.Swarm.SpatialHash.Query")
bool FSwarmSpatialHashTest::RunTest(const FString& Parameters)
{
	FSwarmSpatialHash Hash(200.f);
	Hash.Insert(0, FVector2D(0.0, 0.0));
	Hash.Insert(1, FVector2D(150.0, 0.0));
	Hash.Insert(2, FVector2D(450.0, 0.0));
	Hash.Insert(3, FVector2D(-190.0, -10.0));

	TArray<int32> Ids;
	Hash.Query(FVector2D::ZeroVector, 200.f, Ids);
	Ids.Sort();
	TestEqual(TEXT("跨格与负坐标"), Ids, TArray<int32>({0, 1, 3}));

	Hash.Reset();
	Ids.Reset();
	Hash.Query(FVector2D::ZeroVector, 200.f, Ids);
	TestEqual(TEXT("Reset 后为空"), Ids.Num(), 0);
	return true;
}

SWARM_TEST(FSwarmSteeringSeekTest, "LetsDeepRock.Swarm.Steering.Seek")
bool FSwarmSteeringSeekTest::RunTest(const FString& Parameters)
{
	FSwarmSteeringInput In;
	In.Position = FVector2D::ZeroVector;
	In.TargetPosition = FVector2D(1000.0, 0.0);
	In.bHasTarget = true;
	In.MaxSpeed = 400.f;
	In.DeltaSeconds = 0.1f;
	const FSwarmSteeringResult Result = SwarmComputeSteeringStep(In, [](const FVector2D&) { return false; });
	TestTrue(TEXT("朝目标前进 40uu"), Result.NewPosition.Equals(FVector2D(40.0, 0.0), 0.01));
	TestFalse(TEXT("未被地形阻挡"), Result.bBlockedByTerrain);
	return true;
}

SWARM_TEST(FSwarmSteeringSeparationTest, "LetsDeepRock.Swarm.Steering.Separation")
bool FSwarmSteeringSeparationTest::RunTest(const FString& Parameters)
{
	const TArray<FVector2D> Neighbors = {FVector2D(10.0, 0.0)};
	FSwarmSteeringInput In;
	In.MaxSpeed = 400.f;
	In.DeltaSeconds = 0.1f;
	In.Neighbors = Neighbors;
	const FSwarmSteeringResult Result = SwarmComputeSteeringStep(In, [](const FVector2D&) { return false; });
	TestTrue(TEXT("被邻居推开"), Result.NewPosition.X < 0.0);
	return true;
}

SWARM_TEST(FSwarmSteeringWallSlideTest, "LetsDeepRock.Swarm.Steering.WallSlide")
bool FSwarmSteeringWallSlideTest::RunTest(const FString& Parameters)
{
	FSwarmSteeringInput In;
	In.Position = FVector2D::ZeroVector;
	In.TargetPosition = FVector2D(1000.0, 1000.0);
	In.bHasTarget = true;
	In.MaxSpeed = 400.f;
	In.DeltaSeconds = 0.1f;

	const FSwarmSteeringResult Slide = SwarmComputeSteeringStep(In, [](const FVector2D& P) { return P.X > 5.0; });
	TestTrue(TEXT("X 被挡时仅沿 +Y 滑动"), FMath::IsNearlyZero(Slide.NewPosition.X, 0.01) && Slide.NewPosition.Y > 1.0);
	TestFalse(TEXT("滑动成功不算被阻挡"), Slide.bBlockedByTerrain);

	const FSwarmSteeringResult Stuck = SwarmComputeSteeringStep(In, [&In](const FVector2D& P) { return !P.Equals(In.Position); });
	TestTrue(TEXT("全阻挡时原地不动"), Stuck.NewPosition.Equals(In.Position));
	TestTrue(TEXT("全阻挡标记"), Stuck.bBlockedByTerrain);
	return true;
}

SWARM_TEST(FSwarmSteeringObstacleTest, "LetsDeepRock.Swarm.Steering.ObstacleContact")
bool FSwarmSteeringObstacleTest::RunTest(const FString& Parameters)
{
	const TArray<FSwarmObstacle> Obstacles = {{FVector2D(50.0, 0.0), 40.f}};
	FSwarmSteeringInput In;
	In.Position = FVector2D::ZeroVector;
	In.TargetPosition = FVector2D(50.0, 0.0);
	In.bHasTarget = true;
	In.MaxSpeed = 400.f;
	In.DeltaSeconds = 0.1f;
	In.SelfRadius = 20.f;
	In.Obstacles = Obstacles;
	const FSwarmSteeringResult Result = SwarmComputeSteeringStep(In, [](const FVector2D&) { return false; });
	TestTrue(TEXT("被推出障碍表面"), FVector2D::Distance(Result.NewPosition, FVector2D(50.0, 0.0)) >= 60.0 - 0.01);
	TestTrue(TEXT("标记接触障碍"), Result.bTouchingObstacle);
	return true;
}

SWARM_TEST(FSwarmSteeringNoTargetTest, "LetsDeepRock.Swarm.Steering.NoTargetIdle")
bool FSwarmSteeringNoTargetTest::RunTest(const FString& Parameters)
{
	FSwarmSteeringInput In;
	In.Position = FVector2D(30.0, -20.0);
	In.MaxSpeed = 400.f;
	In.DeltaSeconds = 0.1f;
	const FSwarmSteeringResult Result = SwarmComputeSteeringStep(In, [](const FVector2D&) { return false; });
	TestTrue(TEXT("无目标无邻居时不动"), Result.NewPosition.Equals(In.Position));
	return true;
}

SWARM_TEST(FSwarmFreeSpawnTest, "LetsDeepRock.Swarm.Steering.FreeSpawnPoint")
bool FSwarmFreeSpawnTest::RunTest(const FString& Parameters)
{
	FVector2D Point;
	const bool bFound = SwarmFindFreeSpawnPoint(FVector2D::ZeroVector, 500.f, [](const FVector2D& P) { return P.Size() < 120.0; }, Point);
	TestTrue(TEXT("找到空位"), bFound);
	TestTrue(TEXT("位于阻挡区外且在搜索半径内"), Point.Size() >= 120.0 && Point.Size() <= 500.0 + 0.01);
	TestFalse(TEXT("全阻挡返回 false"), SwarmFindFreeSpawnPoint(FVector2D::ZeroVector, 500.f, [](const FVector2D&) { return true; }, Point));
	return true;
}

#undef SWARM_TEST

#endif // WITH_DEV_AUTOMATION_TESTS
