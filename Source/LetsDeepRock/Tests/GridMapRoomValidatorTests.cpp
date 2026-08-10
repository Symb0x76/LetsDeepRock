// 网格房间模板校验自动化测试

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "GridMap/GridMapRoomValidator.h"

namespace GridMapRoomTests
{
FGridMapRoomExit MakeExit(const TCHAR* Id, const FIntPoint Start, const EGridMapExitDirection Direction, const int32 Width = 1)
{
	FGridMapRoomExit Exit;
	Exit.Id = Id;
	Exit.Start = Start;
	Exit.Direction = Direction;
	Exit.Width = Width;
	return Exit;
}

UGridMapRoomAsset* MakeRoom(const EGridMapRoomType Type, const int32 Width, const int32 Height)
{
	UGridMapRoomAsset* Room = NewObject<UGridMapRoomAsset>(GetTransientPackage());
	Room->RoomType = Type;
	Room->Status = EGridMapRoomStatus::Ready;
	Room->InitializeRoom(Width, Height);
	return Room;
}

bool HasIssue(const TArray<FGridMapValidationIssue>& Issues, const FName Code)
{
	return Issues.ContainsByPredicate([Code](const FGridMapValidationIssue& Issue) { return Issue.Code == Code; });
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGridMapValidConnectorTest, "LetsDeepRock.GridMap.Room.ValidConnector9x9",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGridMapValidConnectorTest::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomTests;
	UGridMapRoomAsset* Room = MakeRoom(EGridMapRoomType::Connector, 9, 9);
	Room->Exits = { MakeExit(TEXT("West"), FIntPoint(0, 4), EGridMapExitDirection::West),
		MakeExit(TEXT("East"), FIntPoint(8, 4), EGridMapExitDirection::East) };
	TestTrue(TEXT("有效连接房可用于生成"), FGridMapRoomValidator::IsValidForGeneration(*Room));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGridMapValidCombatTest, "LetsDeepRock.GridMap.Room.ValidCombat30x30",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGridMapValidCombatTest::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomTests;
	UGridMapRoomAsset* Room = MakeRoom(EGridMapRoomType::Combat, 30, 30);
	Room->Exits.Add(MakeExit(TEXT("South"), FIntPoint(14, 0), EGridMapExitDirection::South, 2));
	FGridMapRoomMarker& Marker = Room->Markers.AddDefaulted_GetRef();
	Marker.Type = EGridMapMarkerType::EnemySpawn;
	Marker.GridCoord = FIntPoint(15, 15);
	TestTrue(TEXT("有效战斗房可用于生成"), FGridMapRoomValidator::IsValidForGeneration(*Room));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGridMapInvalidSizeTest, "LetsDeepRock.GridMap.Room.InvalidSize",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGridMapInvalidSizeTest::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomTests;
	UGridMapRoomAsset* Room = MakeRoom(EGridMapRoomType::Connector, 8, 9);
	Room->Exits = { MakeExit(TEXT("West"), FIntPoint(0, 4), EGridMapExitDirection::West),
		MakeExit(TEXT("East"), FIntPoint(7, 4), EGridMapExitDirection::East) };
	TestTrue(TEXT("报告尺寸错误"), HasIssue(FGridMapRoomValidator::Validate(*Room), TEXT("InvalidSize")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGridMapInvalidExitTest, "LetsDeepRock.GridMap.Room.InvalidExitGeometryAndBlocking",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGridMapInvalidExitTest::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomTests;
	UGridMapRoomAsset* Room = MakeRoom(EGridMapRoomType::Connector, 9, 9);
	Room->Exits = { MakeExit(TEXT("Bad"), FIntPoint(1, 4), EGridMapExitDirection::West),
		MakeExit(TEXT("East"), FIntPoint(8, 4), EGridMapExitDirection::East) };
	Room->SetTile(FIntPoint(8, 4), EGridTileType::Rock);
	const TArray<FGridMapValidationIssue> Issues = FGridMapRoomValidator::Validate(*Room);
	TestTrue(TEXT("报告出口不贴边"), HasIssue(Issues, TEXT("ExitNotOnEdge")));
	TestTrue(TEXT("报告出口覆盖阻挡"), HasIssue(Issues, TEXT("ExitBlocked")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGridMapMissingEnemyTest, "LetsDeepRock.GridMap.Room.CombatMissingEnemy",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGridMapMissingEnemyTest::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomTests;
	UGridMapRoomAsset* Room = MakeRoom(EGridMapRoomType::Combat, 30, 30);
	Room->Exits.Add(MakeExit(TEXT("South"), FIntPoint(15, 0), EGridMapExitDirection::South));
	TestTrue(TEXT("报告战斗房缺少敌人出生点"), HasIssue(FGridMapRoomValidator::Validate(*Room), TEXT("MissingEnemySpawn")));
	return true;
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(FGridMapUnreachableExitTest, "LetsDeepRock.GridMap.Room.UnreachableExit",
	EAutomationTestFlags::EditorContext | EAutomationTestFlags::EngineFilter)
bool FGridMapUnreachableExitTest::RunTest(const FString& Parameters)
{
	using namespace GridMapRoomTests;
	UGridMapRoomAsset* Room = MakeRoom(EGridMapRoomType::Connector, 9, 9);
	Room->Exits = { MakeExit(TEXT("West"), FIntPoint(0, 4), EGridMapExitDirection::West),
		MakeExit(TEXT("East"), FIntPoint(8, 4), EGridMapExitDirection::East) };
	for (int32 Y = 0; Y < 9; ++Y)
	{
		Room->SetTile(FIntPoint(4, Y), EGridTileType::Rock);
	}
	TestTrue(TEXT("报告不可达出口"), HasIssue(FGridMapRoomValidator::Validate(*Room), TEXT("UnreachableExit")));
	return true;
}

#endif
