// GridMap 阶段四 64x64 区块 Renderer 自动化测试

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GridMap/GridMapData.h"
#include "GridMap/GridMapGenerator.h"
#include "GridMap/GridMapRenderer.h"

namespace GridMapRendererChunkTests
{
UGridMapData* Map(const int32 Width = 120, const int32 Height = 120)
{
	UGridMapData* Value = NewObject<UGridMapData>(); FGridMapConfig Config; Config.MapWidth = Width; Config.MapHeight = Height; Value->Initialize(Config); return Value;
}

UGridMapRenderer* Renderer(UGridMapData* MapData)
{
	// 无 World/Owner 时不注册真实组件，只验证同一套区块收集、dirty 合并和实例缓存统计。
	UGridMapRenderer* Value = NewObject<UGridMapRenderer>(); Value->BuildFromMapData(MapData); return Value;
}
}

#define RENDERER_CHUNK_TEST(ClassName, Path) IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, Path, EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

RENDERER_CHUNK_TEST(FGridRendererUncarvedEmpty, "LetsDeepRock.GridMap.Stage4B.UncarvedAndEmptyNoInstances")
bool FGridRendererUncarvedEmpty::RunTest(const FString& Parameters)
{
	using namespace GridMapRendererChunkTests; UGridMapData* M = Map(); M->Fill(EGridTileType::Uncarved); UGridMapRenderer* R = Renderer(M);
	TestEqual(TEXT("Uncarved 零实例"), R->GetTotalInstanceCount(), 0); TestEqual(TEXT("空区块不进入活动缓存"), R->GetActiveChunkCount(), 0);
	TestEqual(TEXT("无活动类型不创建 HISM"), R->GetHISMComponentCount(), 0); return true;
}

RENDERER_CHUNK_TEST(FGridRendererExplicitTypes, "LetsDeepRock.GridMap.Stage4B.ExplicitTypeCounts")
bool FGridRendererExplicitTypes::RunTest(const FString& Parameters)
{
	using namespace GridMapRendererChunkTests; UGridMapData* M = Map(); NewObject<UGridMapGenerator_Test>()->Generate(M); UGridMapRenderer* R = Renderer(M);
	TestEqual(TEXT("Tree 数量"), R->GetInstanceCountForType(EGridTileType::Tree), 1); TestEqual(TEXT("Rock 数量"), R->GetInstanceCountForType(EGridTileType::Rock), 1);
	TestEqual(TEXT("LargeTree 数量"), R->GetInstanceCountForType(EGridTileType::LargeTree), 1); TestEqual(TEXT("实例总数等于三类显式格"), R->GetTotalInstanceCount(), 3);
	TestEqual(TEXT("三个格位于同一活动区块"), R->GetActiveChunkCount(), 1); TestEqual(TEXT("无 Owner 的测试不伪造已注册组件"), R->GetHISMComponentCount(), 0); return true;
}

RENDERER_CHUNK_TEST(FGridRendererDirtyMerge, "LetsDeepRock.GridMap.Stage4B.DirtyChunkMerge")
bool FGridRendererDirtyMerge::RunTest(const FString& Parameters)
{
	using namespace GridMapRendererChunkTests; UGridMapRenderer* R = Renderer(Map());
	R->MarkDirtyChunk(1, 1); TestEqual(TEXT("单格只标脏一个区块"), R->GetDirtyChunkCount(), 1);
	R->MarkDirtyChunk(2, 2); R->MarkDirtyChunk(63, 63); TestEqual(TEXT("同区块多次修改合并"), R->GetDirtyChunkCount(), 1);
	R->FlushDirtyChunks(); TestEqual(TEXT("同区块批量只刷新一次"), R->GetLastFlushChunkCount(), 1); TestEqual(TEXT("Flush 清空 dirty"), R->GetDirtyChunkCount(), 0); return true;
}

RENDERER_CHUNK_TEST(FGridRendererCrossChunkDirty, "LetsDeepRock.GridMap.Stage4B.CrossChunkDirty")
bool FGridRendererCrossChunkDirty::RunTest(const FString& Parameters)
{
	using namespace GridMapRendererChunkTests; UGridMapRenderer* R = Renderer(Map(130, 130));
	R->MarkDirtyChunk(0, 0); R->MarkDirtyChunk(64, 0); R->MarkDirtyChunk(129, 129); R->MarkDirtyChunk(65, 1);
	TestEqual(TEXT("跨区块标脏相应数量"), R->GetDirtyChunkCount(), 3); R->FlushDirtyChunks(); TestEqual(TEXT("跨区块刷新相应数量"), R->GetLastFlushChunkCount(), 3); return true;
}

#undef RENDERER_CHUNK_TEST
#endif
