// GridMap 阶段四 4C 导航脏区块自动化测试

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GridMap/GridMapTypes.h"

#define STAGE4C_NAV_TEST(ClassName, Path) IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, Path, EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

STAGE4C_NAV_TEST(FGridNavigationSingleChunk, "LetsDeepRock.GridMap.Stage4C.SingleChunk")
bool FGridNavigationSingleChunk::RunTest(const FString& Parameters)
{
	FGridNavigationDirtyTracker Tracker;
	Tracker.MarkTile(1, 1, true);
	TestEqual(TEXT("单格标脏一个导航区块"), Tracker.Num(), 1);
	return true;
}

STAGE4C_NAV_TEST(FGridNavigationSameChunkMerge, "LetsDeepRock.GridMap.Stage4C.SameChunkMerge")
bool FGridNavigationSameChunkMerge::RunTest(const FString& Parameters)
{
	FGridNavigationDirtyTracker Tracker;
	Tracker.MarkTile(0, 0, true);
	Tracker.MarkTile(63, 63, true);
	Tracker.MarkTile(12, 47, true);
	TestEqual(TEXT("同一 64x64 区块合并"), Tracker.Num(), 1);
	return true;
}

STAGE4C_NAV_TEST(FGridNavigationCrossChunk, "LetsDeepRock.GridMap.Stage4C.CrossChunk")
bool FGridNavigationCrossChunk::RunTest(const FString& Parameters)
{
	FGridNavigationDirtyTracker Tracker;
	Tracker.MarkTile(0, 0, true);
	Tracker.MarkTile(64, 0, true);
	Tracker.MarkTile(129, 129, true);
	Tracker.MarkTile(65, 1, true);
	TestEqual(TEXT("跨区块保留三个脏区块"), Tracker.Num(), 3);
	return true;
}

STAGE4C_NAV_TEST(FGridNavigationFlushClears, "LetsDeepRock.GridMap.Stage4C.FlushClears")
bool FGridNavigationFlushClears::RunTest(const FString& Parameters)
{
	FGridNavigationDirtyTracker Tracker;
	Tracker.MarkTile(0, 0, true);
	Tracker.MarkTile(64, 0, true);
	int32 Submitted = 0;
	const int32 Flushed = Tracker.Flush(true, [&Submitted](const FIntPoint&) { ++Submitted; });
	TestEqual(TEXT("Flush 返回区块数"), Flushed, 2);
	TestEqual(TEXT("每个区块只提交一次"), Submitted, 2);
	TestEqual(TEXT("Flush 后清空"), Tracker.Num(), 0);
	return true;
}

STAGE4C_NAV_TEST(FGridNavigationClientDoesNothing, "LetsDeepRock.GridMap.Stage4C.ClientDoesNotAccumulateOrSubmit")
bool FGridNavigationClientDoesNothing::RunTest(const FString& Parameters)
{
	FGridNavigationDirtyTracker Tracker;
	Tracker.MarkTile(1, 1, false);
	Tracker.MarkTile(64, 64, false);
	int32 Submitted = 0;
	const int32 Flushed = Tracker.Flush(false, [&Submitted](const FIntPoint&) { ++Submitted; });
	TestEqual(TEXT("客户端不积累"), Tracker.Num(), 0);
	TestEqual(TEXT("客户端不 Flush 区块"), Flushed, 0);
	TestEqual(TEXT("客户端不提交"), Submitted, 0);
	return true;
}

#undef STAGE4C_NAV_TEST
#endif
