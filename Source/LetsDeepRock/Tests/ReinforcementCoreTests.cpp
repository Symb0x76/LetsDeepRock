// 增援/倒地纯规则自动化测试

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "UObject/Package.h"
#include "Reinforcement/WeaponReinforcementRules.h"

#define REINFORCE_TEST(ClassName, Path) IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, Path, EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

REINFORCE_TEST(FReinforcePoolSizeTest, "LetsDeepRock.Reinforcement.Rules.PoolSize")
bool FReinforcePoolSizeTest::RunTest(const FString& Parameters)
{
	const TArray<int32> None;
	TestEqual(TEXT("默认 难度1"), WeaponReinforcementPoolForDifficulty(1, None), 8);
	TestEqual(TEXT("默认 难度2"), WeaponReinforcementPoolForDifficulty(2, None), 6);
	TestEqual(TEXT("默认 难度3"), WeaponReinforcementPoolForDifficulty(3, None), 4);
	TestEqual(TEXT("难度0 夹到1"), WeaponReinforcementPoolForDifficulty(0, None), 8);
	TestEqual(TEXT("难度9 夹到3"), WeaponReinforcementPoolForDifficulty(9, None), 4);
	TestEqual(TEXT("自定义 难度2"), WeaponReinforcementPoolForDifficulty(2, TArray<int32>{ 5, 3, 1 }), 3);
	TestEqual(TEXT("过短数组回退默认"), WeaponReinforcementPoolForDifficulty(2, TArray<int32>{ 7 }), 6);
	TestEqual(TEXT("负值夹到0"), WeaponReinforcementPoolForDifficulty(1, TArray<int32>{ -4, 3, 1 }), 0);
	return true;
}

REINFORCE_TEST(FReinforceDownedChoiceTest, "LetsDeepRock.Reinforcement.Rules.DownedChoice")
bool FReinforceDownedChoiceTest::RunTest(const FString& Parameters)
{
	TestFalse(TEXT("无队友且要求救援者"), WeaponShouldEnterDowned(0, true));
	TestTrue(TEXT("有队友"), WeaponShouldEnterDowned(1, true));
	TestTrue(TEXT("不要求救援者"), WeaponShouldEnterDowned(0, false));
	return true;
}

REINFORCE_TEST(FReinforceDamageSecondsTest, "LetsDeepRock.Reinforcement.Rules.DamageSeconds")
bool FReinforceDamageSecondsTest::RunTest(const FString& Parameters)
{
	TestEqual(TEXT("10%"), WeaponDownedDamageToSeconds(10.f, 100.f, 10.f), 1.f);
	TestEqual(TEXT("100%"), WeaponDownedDamageToSeconds(100.f, 100.f, 10.f), 10.f);
	TestEqual(TEXT("MaxHealth 为 0"), WeaponDownedDamageToSeconds(5.f, 0.f, 10.f), 10.f);
	TestEqual(TEXT("负伤害"), WeaponDownedDamageToSeconds(-3.f, 100.f, 10.f), 0.f);
	return true;
}

REINFORCE_TEST(FReinforceFailRuleTest, "LetsDeepRock.Reinforcement.Rules.FailRule")
bool FReinforceFailRuleTest::RunTest(const FString& Parameters)
{
	TestTrue(TEXT("池空且无人存活"), WeaponShouldFailMission(0, 0, 2));
	TestFalse(TEXT("池未空"), WeaponShouldFailMission(1, 0, 2));
	TestFalse(TEXT("仍有人存活"), WeaponShouldFailMission(0, 1, 2));
	TestFalse(TEXT("无连接玩家"), WeaponShouldFailMission(0, 0, 0));
	return true;
}

REINFORCE_TEST(FReinforceReviveTrackerTest, "LetsDeepRock.Reinforcement.Rules.ReviveTracker")
bool FReinforceReviveTrackerTest::RunTest(const FString& Parameters)
{
	// UObject 为抽象类,不能直接 NewObject;用两个临时 UPackage 作为互不相同的目标
	const UObject* A = NewObject<UPackage>(nullptr, TEXT("/Temp/ReinforceTrackerA"), RF_Transient);
	const UObject* B = NewObject<UPackage>(nullptr, TEXT("/Temp/ReinforceTrackerB"), RF_Transient);
	const float Secs = 3.f;

	FWeaponReviveTracker T;
	TestFalse(TEXT("第1秒"), T.Advance(A, true, 1.f, Secs));
	TestFalse(TEXT("第2秒"), T.Advance(A, true, 1.f, Secs));
	TestTrue(TEXT("进度约 0.667"), FMath::IsNearlyEqual(T.GetProgress(Secs), 2.f / 3.f, 0.001f));
	TestTrue(TEXT("第3秒完成"), T.Advance(A, true, 1.f, Secs));
	TestEqual(TEXT("完成后复位"), T.GetProgress(Secs), 0.f);

	T.Advance(A, true, 2.f, Secs);
	TestFalse(TEXT("换目标重来"), T.Advance(B, true, 2.f, Secs));
	TestTrue(TEXT("换目标后进度 0.667"), FMath::IsNearlyEqual(T.GetProgress(Secs), 2.f / 3.f, 0.001f));
	TestTrue(TEXT("目标为 B"), T.GetTarget() == B);

	TestFalse(TEXT("松手"), T.Advance(B, false, 0.1f, Secs));
	TestEqual(TEXT("松手归零"), T.GetProgress(Secs), 0.f);
	TestFalse(TEXT("空目标"), T.Advance(nullptr, true, 5.f, Secs));
	return true;
}

REINFORCE_TEST(FReinforceQueueRankTest, "LetsDeepRock.Reinforcement.Rules.ReinforceQueueRank")
bool FReinforceQueueRankTest::RunTest(const FString& Parameters)
{
	// 条目 = (RespawnServerTime, PlayerId);计时器按时间先后触发,同时刻按 id 先后
	const TArray<TPair<float, int32>> Pending = { {30.f, 7}, {10.f, 5}, {20.f, 9}, {20.f, 3} };
	TestEqual(TEXT("最早的时间排第 0"), WeaponReinforceQueueRank(Pending, 5), 0);
	TestEqual(TEXT("同时刻 id 小者在前"), WeaponReinforceQueueRank(Pending, 3), 1);
	TestEqual(TEXT("同时刻 id 大者在后"), WeaponReinforceQueueRank(Pending, 9), 2);
	TestEqual(TEXT("最晚的排最后"), WeaponReinforceQueueRank(Pending, 7), 3);
	TestEqual(TEXT("自己不在队列里 → INDEX_NONE"), WeaponReinforceQueueRank(Pending, 42), static_cast<int32>(INDEX_NONE));
	TestEqual(TEXT("空队列 → INDEX_NONE"), WeaponReinforceQueueRank(TArray<TPair<float, int32>>(), 1), static_cast<int32>(INDEX_NONE));
	return true;
}

#undef REINFORCE_TEST

#endif // WITH_DEV_AUTOMATION_TESTS
