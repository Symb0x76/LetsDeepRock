// GridMap 阶段三房间图自动化测试

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GridMap/GridMapData.h"
#include "GridMap/GridMapGenerator_RoomGraph.h"

namespace GridMapRoomGraphTests
{
FGridMapRoomExit Exit(const TCHAR* Id, const FIntPoint Start, const EGridMapExitDirection Direction)
{
	FGridMapRoomExit Value; Value.Id = Id; Value.Start = Start; Value.Direction = Direction; return Value;
}

UGridMapRoomAsset* Room(UObject* Outer, const TCHAR* Name, const EGridMapRoomType Type, const bool bSpawn = false,
	const bool bDraft = false, const int32 Version = 1)
{
	const int32 Size = Type == EGridMapRoomType::Connector ? 9 : 30;
	UGridMapRoomAsset* Value = NewObject<UGridMapRoomAsset>(Outer, MakeUniqueObjectName(Outer, UGridMapRoomAsset::StaticClass(), Name));
	Value->RoomType = Type; Value->Status = bDraft ? EGridMapRoomStatus::Draft : EGridMapRoomStatus::Ready;
	Value->TemplateVersion = Version; Value->InitializeRoom(Size, Size);
	Value->Exits = { Exit(TEXT("W"), {0, Size / 2}, EGridMapExitDirection::West),
		Exit(TEXT("E"), {Size - 1, Size / 2}, EGridMapExitDirection::East) };
	if (Type == EGridMapRoomType::Connector)
	{
		Value->Exits.Add(Exit(TEXT("S"), {Size / 2, 0}, EGridMapExitDirection::South));
		Value->Exits.Add(Exit(TEXT("N"), {Size / 2, Size - 1}, EGridMapExitDirection::North));
	}
	if (bSpawn) { FGridMapRoomMarker& M = Value->Markers.AddDefaulted_GetRef(); M.Type = EGridMapMarkerType::PlayerSpawn; M.GridCoord = {4, 4}; }
	if (Type == EGridMapRoomType::Combat) { FGridMapRoomMarker& M = Value->Markers.AddDefaulted_GetRef(); M.Type = EGridMapMarkerType::EnemySpawn; M.GridCoord = {15, 15}; }
	if (Type == EGridMapRoomType::Objective) { FGridMapRoomMarker& M = Value->Markers.AddDefaulted_GetRef(); M.Type = EGridMapMarkerType::Objective; M.GridCoord = {15, 15}; }
	return Value;
}

UGridMapGenerator_RoomGraph* Generator(const int32 Seed, const bool bIncludeDraft = false)
{
	UGridMapGenerator_RoomGraph* Value = NewObject<UGridMapGenerator_RoomGraph>(); Value->SetEffectiveSeed(Seed);
	Value->TransientTemplates = { Room(Value, TEXT("Stage3Spawn"), EGridMapRoomType::Connector, true),
		Room(Value, TEXT("Stage3Connector"), EGridMapRoomType::Connector), Room(Value, TEXT("Stage3Combat"), EGridMapRoomType::Combat),
		Room(Value, TEXT("Stage3Objective"), EGridMapRoomType::Objective) };
	if (bIncludeDraft) { UGridMapRoomAsset* Draft = Room(Value, TEXT("Stage3Draft"), EGridMapRoomType::Connector, false, true); Draft->Weight = 100000.0f; Value->TransientTemplates.Add(Draft); }
	return Value;
}

UGridMapData* Map()
{
	UGridMapData* Value = NewObject<UGridMapData>(); FGridMapConfig Config; Config.MapWidth = 160; Config.MapHeight = 80; Value->Initialize(Config); return Value;
}

uint64 GenerateHash(UGridMapGenerator_RoomGraph* GeneratorValue, UGridMapData* MapValue)
{
	if (!GeneratorValue->GenerateChecked(MapValue)) { return 0; }
	return UGridMapGenerator::ComputeStableResultHash(*MapValue, GeneratorValue->GetEffectiveSeed(),
		GeneratorValue->GetAlgorithmVersion(), GeneratorValue->GetTemplateSetVersion(), GeneratorValue->GetGeneratedRecipe());
}

bool RecipesDiffer(const FGridMapGenerationRecipe& A, const FGridMapGenerationRecipe& B)
{
	if (A.Rooms.Num() != B.Rooms.Num() || A.Connections.Num() != B.Connections.Num()) { return true; }
	for (int32 Index = 0; Index < A.Rooms.Num(); ++Index) { if (A.Rooms[Index].Origin != B.Rooms[Index].Origin || A.Rooms[Index].TemplatePath != B.Rooms[Index].TemplatePath) { return true; } }
	for (int32 Index = 0; Index < A.Connections.Num(); ++Index) { if (A.Connections[Index].FromRoom != B.Connections[Index].FromRoom || A.Connections[Index].Points != B.Connections[Index].Points) { return true; } }
	return false;
}
}

#define ROOM_GRAPH_TEST(ClassName, Path) IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, Path, EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)

ROOM_GRAPH_TEST(FGridMapRoomGraphStableHash, "LetsDeepRock.GridMap.Stage3.FixedSeedStableHash")
bool FGridMapRoomGraphStableHash::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomGraphTests; UGridMapGenerator_RoomGraph* G = Generator(123);
	const uint64 HashA = GenerateHash(G, Map()); const uint64 HashB = GenerateHash(G, Map()); TestTrue(TEXT("第一次生成成功且哈希非零"), HashA != 0); TestTrue(TEXT("第二次生成成功且哈希非零"), HashB != 0); TestEqual(TEXT("固定 Seed 哈希稳定"), HashA, HashB); return true;
}

ROOM_GRAPH_TEST(FGridMapRoomGraphDifferentSeed, "LetsDeepRock.GridMap.Stage3.DifferentSeedDifferentHash")
bool FGridMapRoomGraphDifferentSeed::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomGraphTests; UGridMapGenerator_RoomGraph* G = Generator(123); TestTrue(TEXT("Seed 123 生成成功"), G->GenerateChecked(Map())); const FGridMapGenerationRecipe First = G->GetGeneratedRecipe();
	G->SetEffectiveSeed(124); TestTrue(TEXT("Seed 124 生成成功"), G->GenerateChecked(Map())); TestTrue(TEXT("不同 Seed 改变 Recipe 布局"), RecipesDiffer(First, G->GetGeneratedRecipe())); return true;
}

ROOM_GRAPH_TEST(FGridMapRoomGraphReachable, "LetsDeepRock.GridMap.Stage3.SpawnToObjectiveReachable")
bool FGridMapRoomGraphReachable::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomGraphTests; UGridMapGenerator_RoomGraph* G = Generator(7); TestTrue(TEXT("生成成功"), G->GenerateChecked(Map())); TestTrue(TEXT("出生到目标可达"), G->IsLastTopologyReachable()); return true;
}

ROOM_GRAPH_TEST(FGridMapRoomGraphBounds, "LetsDeepRock.GridMap.Stage3.BoundsNoOverlap")
bool FGridMapRoomGraphBounds::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomGraphTests; UGridMapGenerator_RoomGraph* G = Generator(8); TestTrue(TEXT("生成成功"), G->GenerateChecked(Map()));
	const TArray<FIntRect>& Bounds = G->GetLastRoomBounds(); for (int32 A = 0; A < Bounds.Num(); ++A) { TestTrue(TEXT("房间不越界"), Bounds[A].Min.X >= 0 && Bounds[A].Min.Y >= 0 && Bounds[A].Max.X <= 160 && Bounds[A].Max.Y <= 80); for (int32 B = A + 1; B < Bounds.Num(); ++B) { TestFalse(TEXT("房间不重叠"), Bounds[A].Intersect(Bounds[B])); } } return true;
}

ROOM_GRAPH_TEST(FGridMapRoomGraphRejectDraft, "LetsDeepRock.GridMap.Stage3.DraftNotSelected")
bool FGridMapRoomGraphRejectDraft::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomGraphTests; UGridMapGenerator_RoomGraph* G = Generator(9, true); TestTrue(TEXT("生成成功"), G->GenerateChecked(Map()));
	for (const FGridMapRecipeRoom& R : G->GetGeneratedRecipe().Rooms) { TestFalse(TEXT("不选择 Draft"), R.TemplatePath.ToString().Contains(TEXT("Stage3Draft"))); } return true;
}

ROOM_GRAPH_TEST(FGridMapRoomGraphMissingRequired, "LetsDeepRock.GridMap.Stage3.MissingRequiredFails")
bool FGridMapRoomGraphMissingRequired::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomGraphTests; UGridMapGenerator_RoomGraph* G = Generator(10); G->TransientTemplates.RemoveAt(3); TestFalse(TEXT("缺 Objective 明确失败"), G->GenerateChecked(Map())); TestTrue(TEXT("包含失败原因"), !G->GetLastError().IsEmpty()); return true;
}

ROOM_GRAPH_TEST(FGridMapRoomGraphRecipeReplay, "LetsDeepRock.GridMap.Stage3.RecipeReplaySameMap")
bool FGridMapRoomGraphRecipeReplay::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomGraphTests; UGridMapGenerator_RoomGraph* G = Generator(11); UGridMapData* MapA = Map(); const uint64 HashA = GenerateHash(G, MapA);
	G->SetReplayRecipe(G->GetGeneratedRecipe()); UGridMapData* MapB = Map(); const uint64 HashB = GenerateHash(G, MapB); TestEqual(TEXT("Recipe 重放同图"), HashA, HashB); return true;
}

ROOM_GRAPH_TEST(FGridMapRoomGraphCorridorConnected, "LetsDeepRock.GridMap.Stage3.CorridorExitsConnected")
bool FGridMapRoomGraphCorridorConnected::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomGraphTests; UGridMapGenerator_RoomGraph* G = Generator(12); UGridMapData* M = Map(); TestTrue(TEXT("生成成功"), G->GenerateChecked(M));
	for (const FGridMapRecipeConnection& C : G->GetGeneratedRecipe().Connections) { TestTrue(TEXT("连接含起终点"), C.Points.Num() >= 2); for (int32 Segment = 1; Segment < C.Points.Num(); ++Segment) { const FIntPoint A = C.Points[Segment - 1], B = C.Points[Segment]; const FIntPoint Step(FMath::Sign(B.X - A.X), FMath::Sign(B.Y - A.Y)); FIntPoint P = A; for (;;) { const FGridTileData* Tile = M->GetTile(P.X, P.Y); TestTrue(TEXT("完整走廊为空地"), Tile && Tile->IsPassable()); if (P == B) { break; } P += Step; } } } return true;
}

ROOM_GRAPH_TEST(FGridMapRoomGraphRejectInvalidReady, "LetsDeepRock.GridMap.Stage3.InvalidReadyNotSelected")
bool FGridMapRoomGraphRejectInvalidReady::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomGraphTests; UGridMapGenerator_RoomGraph* G = Generator(13); UGridMapRoomAsset* Invalid = Room(G, TEXT("Stage3InvalidReady"), EGridMapRoomType::Connector); Invalid->Weight = 100000.0f; Invalid->Exits.Reset(); G->TransientTemplates.Add(Invalid);
	TestTrue(TEXT("生成成功"), G->GenerateChecked(Map())); for (const FGridMapRecipeRoom& R : G->GetGeneratedRecipe().Rooms) { TestFalse(TEXT("不选择无效 Ready"), R.TemplatePath.ToString().Contains(TEXT("Stage3InvalidReady"))); } return true;
}

ROOM_GRAPH_TEST(FGridMapRoomGraphWeightSelection, "LetsDeepRock.GridMap.Stage3.WeightSelection")
bool FGridMapRoomGraphWeightSelection::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomGraphTests; UGridMapGenerator_RoomGraph* G = Generator(14); UGridMapRoomAsset* Zero = Room(G, TEXT("Stage3ZeroWeight"), EGridMapRoomType::Connector); Zero->Weight = 0.0f; UGridMapRoomAsset* Heavy = Room(G, TEXT("Stage3HeavyWeight"), EGridMapRoomType::Connector); Heavy->Weight = 1000000.0f; G->TransientTemplates.Add(Zero); G->TransientTemplates.Add(Heavy);
	TestTrue(TEXT("生成成功"), G->GenerateChecked(Map())); bool bHeavySelected = false; for (const FGridMapRecipeRoom& R : G->GetGeneratedRecipe().Rooms) { TestFalse(TEXT("零权重永不选择"), R.TemplatePath.ToString().Contains(TEXT("Stage3ZeroWeight"))); bHeavySelected |= R.TemplatePath.ToString().Contains(TEXT("Stage3HeavyWeight")); } TestTrue(TEXT("固定 Seed 选择高权重模板"), bHeavySelected); return true;
}

ROOM_GRAPH_TEST(FGridMapRoomGraphTemplateHashRejectsChange, "LetsDeepRock.GridMap.Stage3.TemplateContentHashRejectsChange")
bool FGridMapRoomGraphTemplateHashRejectsChange::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomGraphTests; UGridMapGenerator_RoomGraph* A = Generator(15); TestTrue(TEXT("初次生成成功"), A->GenerateChecked(Map())); const FGridMapGenerationRecipe Recipe = A->GetGeneratedRecipe(); A->TransientTemplates[0]->SetTile({1, 1}, EGridTileType::Tree); A->SetReplayRecipe(Recipe); TestFalse(TEXT("模板内容变化拒绝重放"), A->GenerateChecked(Map())); return true;
}

#undef ROOM_GRAPH_TEST
#endif
