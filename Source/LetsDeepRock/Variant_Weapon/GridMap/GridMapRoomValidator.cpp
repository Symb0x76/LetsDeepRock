// 网格房间模板纯数据校验器

#include "GridMapRoomValidator.h"

namespace GridMapRoomValidation
{
void AddIssue(TArray<FGridMapValidationIssue>& Issues, const EGridMapValidationSeverity Severity,
	const FName Code, const FString& Message, const FIntPoint Coord = FIntPoint(INDEX_NONE, INDEX_NONE))
{
	FGridMapValidationIssue& Issue = Issues.AddDefaulted_GetRef();
	Issue.Severity = Severity;
	Issue.Code = Code;
	Issue.Message = Message;
	Issue.GridCoord = Coord;
}

FIntPoint ExitCoord(const FGridMapRoomExit& Exit, const int32 Offset)
{
	return Exit.Direction == EGridMapExitDirection::North || Exit.Direction == EGridMapExitDirection::South
		? Exit.Start + FIntPoint(Offset, 0)
		: Exit.Start + FIntPoint(0, Offset);
}

bool IsOnExpectedEdge(const UGridMapRoomAsset& Room, const FGridMapRoomExit& Exit)
{
	switch (Exit.Direction)
	{
	case EGridMapExitDirection::North: return Exit.Start.Y == Room.Size.Y - 1;
	case EGridMapExitDirection::East: return Exit.Start.X == Room.Size.X - 1;
	case EGridMapExitDirection::South: return Exit.Start.Y == 0;
	case EGridMapExitDirection::West: return Exit.Start.X == 0;
	default: return false;
	}
}

FIntPoint InwardStep(const EGridMapExitDirection Direction)
{
	switch (Direction)
	{
	case EGridMapExitDirection::North: return FIntPoint(0, -1);
	case EGridMapExitDirection::East: return FIntPoint(-1, 0);
	case EGridMapExitDirection::South: return FIntPoint(0, 1);
	case EGridMapExitDirection::West: return FIntPoint(1, 0);
	default: return FIntPoint::ZeroValue;
	}
}

bool IsEmpty(const UGridMapRoomAsset& Room, const FIntPoint Coord)
{
	const int32 Index = Room.CoordToIndex(Coord);
	return Room.Tiles.IsValidIndex(Index) && Room.Tiles[Index] == EGridTileType::Empty;
}
}

TArray<FGridMapValidationIssue> FGridMapRoomValidator::Validate(const UGridMapRoomAsset& Room)
{
	using namespace GridMapRoomValidation;
	TArray<FGridMapValidationIssue> Issues;
	const bool bSizeValid = Room.RoomType == EGridMapRoomType::Connector
		? Room.Size.X >= 9 && Room.Size.X <= 15 && Room.Size.Y >= 9 && Room.Size.Y <= 15
		: Room.Size.X >= 30 && Room.Size.Y >= 30;
	if (!bSizeValid)
	{
		AddIssue(Issues, EGridMapValidationSeverity::Error, TEXT("InvalidSize"), TEXT("房间尺寸不符合类型要求。"));
	}
	if (Room.Size.X <= 0 || Room.Size.Y <= 0 || Room.Tiles.Num() != Room.Size.X * Room.Size.Y)
	{
		AddIssue(Issues, EGridMapValidationSeverity::Error, TEXT("TileCountMismatch"), TEXT("格子数量与房间尺寸不匹配。"));
	}

	const int32 MinimumExitCount = Room.RoomType == EGridMapRoomType::Connector ? 2 : 1;
	if (Room.Exits.Num() < MinimumExitCount)
	{
		AddIssue(Issues, EGridMapValidationSeverity::Error, TEXT("InsufficientExits"), TEXT("房间出口数量不足。"));
	}

	TSet<FName> ExitIds;
	bool bExitGeometryValid = true;
	for (const FGridMapRoomExit& Exit : Room.Exits)
	{
		if (Exit.Id.IsNone() || ExitIds.Contains(Exit.Id))
		{
			AddIssue(Issues, EGridMapValidationSeverity::Error, TEXT("InvalidExitId"), TEXT("出口 ID 必须非空且唯一。"), Exit.Start);
		}
		ExitIds.Add(Exit.Id);
		if (Exit.Width <= 0 || !IsOnExpectedEdge(Room, Exit))
		{
			AddIssue(Issues, EGridMapValidationSeverity::Error, TEXT("ExitNotOnEdge"), TEXT("出口未贴在方向对应的边缘。"), Exit.Start);
			bExitGeometryValid = false;
		}
		for (int32 Offset = 0; Offset < FMath::Max(Exit.Width, 1); ++Offset)
		{
			const FIntPoint Coord = ExitCoord(Exit, Offset);
			if (!Room.IsInBounds(Coord))
			{
				AddIssue(Issues, EGridMapValidationSeverity::Error, TEXT("ExitOutOfBounds"), TEXT("出口宽度越出房间边界。"), Coord);
				bExitGeometryValid = false;
			}
			else if (!IsEmpty(Room, Coord))
			{
				AddIssue(Issues, EGridMapValidationSeverity::Error, TEXT("ExitBlocked"), TEXT("出口覆盖格必须为空地。"), Coord);
			}
		}
	}

	bool bHasEnemySpawn = false;
	bool bHasObjective = false;
	for (const FGridMapRoomMarker& Marker : Room.Markers)
	{
		bHasEnemySpawn |= Marker.Type == EGridMapMarkerType::EnemySpawn;
		bHasObjective |= Marker.Type == EGridMapMarkerType::Objective;
		if (!Room.IsInBounds(Marker.GridCoord))
		{
			AddIssue(Issues, EGridMapValidationSeverity::Error, TEXT("MarkerOutOfBounds"), TEXT("语义标记位于房间边界外。"), Marker.GridCoord);
		}
		else if (!IsEmpty(Room, Marker.GridCoord))
		{
			AddIssue(Issues, EGridMapValidationSeverity::Error, TEXT("MarkerBlocked"), TEXT("语义标记必须位于空地。"), Marker.GridCoord);
		}
	}
	if (Room.RoomType == EGridMapRoomType::Combat && !bHasEnemySpawn)
	{
		AddIssue(Issues, EGridMapValidationSeverity::Error, TEXT("MissingEnemySpawn"), TEXT("战斗房必须包含敌人出生点。"));
	}
	if (Room.RoomType == EGridMapRoomType::Objective && !bHasObjective)
	{
		AddIssue(Issues, EGridMapValidationSeverity::Error, TEXT("MissingObjective"), TEXT("目标房必须包含目标点。"));
	}

	// 从第一个出口的内侧空地执行四方向遍历。
	TSet<FIntPoint> Reachable;
	if (Room.Exits.Num() > 0 && bExitGeometryValid && Room.Tiles.Num() == Room.Size.X * Room.Size.Y)
	{
		const FIntPoint Start = Room.Exits[0].Start + InwardStep(Room.Exits[0].Direction);
		if (!IsEmpty(Room, Start))
		{
			AddIssue(Issues, EGridMapValidationSeverity::Error, TEXT("ExitInteriorBlocked"), TEXT("第一个出口的内侧格必须为空地。"), Start);
		}
		else
		{
			TArray<FIntPoint> Queue;
			Queue.Add(Start);
			Reachable.Add(Start);
			for (int32 Head = 0; Head < Queue.Num(); ++Head)
			{
				static const FIntPoint Steps[] = { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) };
				for (const FIntPoint Step : Steps)
				{
					const FIntPoint Next = Queue[Head] + Step;
					if (IsEmpty(Room, Next) && !Reachable.Contains(Next))
					{
						Reachable.Add(Next);
						Queue.Add(Next);
					}
				}
			}
		}
		for (const FGridMapRoomExit& Exit : Room.Exits)
		{
			for (int32 Offset = 0; Offset < Exit.Width; ++Offset)
			{
				const FIntPoint Coord = ExitCoord(Exit, Offset);
				if (IsEmpty(Room, Coord) && !Reachable.Contains(Coord))
				{
					AddIssue(Issues, EGridMapValidationSeverity::Error, TEXT("UnreachableExit"), TEXT("出口无法从第一个出口到达。"), Coord);
					break;
				}
			}
		}
		for (const FGridMapRoomMarker& Marker : Room.Markers)
		{
			const bool bRequired = (Room.RoomType == EGridMapRoomType::Combat && Marker.Type == EGridMapMarkerType::EnemySpawn)
				|| (Room.RoomType == EGridMapRoomType::Objective && Marker.Type == EGridMapMarkerType::Objective);
			if (bRequired && IsEmpty(Room, Marker.GridCoord) && !Reachable.Contains(Marker.GridCoord))
			{
				AddIssue(Issues, EGridMapValidationSeverity::Error, TEXT("UnreachableRequiredMarker"), TEXT("必需标记无法从第一个出口到达。"), Marker.GridCoord);
			}
		}

		// 对每个未到达空地区域计数，三格及以上给出警告。
		TSet<FIntPoint> Counted = Reachable;
		for (int32 Y = 0; Y < Room.Size.Y; ++Y)
		{
			for (int32 X = 0; X < Room.Size.X; ++X)
			{
				const FIntPoint RegionStart(X, Y);
				if (!IsEmpty(Room, RegionStart) || Counted.Contains(RegionStart))
				{
					continue;
				}
				TArray<FIntPoint> RegionQueue;
				RegionQueue.Add(RegionStart);
				Counted.Add(RegionStart);
				for (int32 Head = 0; Head < RegionQueue.Num(); ++Head)
				{
					static const FIntPoint Steps[] = { FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1) };
					for (const FIntPoint Step : Steps)
					{
						const FIntPoint Next = RegionQueue[Head] + Step;
						if (IsEmpty(Room, Next) && !Counted.Contains(Next))
						{
							Counted.Add(Next);
							RegionQueue.Add(Next);
						}
					}
				}
				if (RegionQueue.Num() >= 3)
				{
					AddIssue(Issues, EGridMapValidationSeverity::Warning, TEXT("UnreachableEmptyRegion"), TEXT("存在至少三格的不可达空地区域。"), RegionStart);
				}
			}
		}
	}

	if (Room.Status == EGridMapRoomStatus::Ready && Issues.ContainsByPredicate([](const FGridMapValidationIssue& Issue)
		{ return Issue.Severity == EGridMapValidationSeverity::Error; }))
	{
		AddIssue(Issues, EGridMapValidationSeverity::Error, TEXT("ReadyAssetInvalid"), TEXT("Ready 房间包含错误，不能用于生成。"));
	}
	return Issues;
}

bool FGridMapRoomValidator::IsValidForGeneration(const UGridMapRoomAsset& Room, TArray<FGridMapValidationIssue>* OutIssues)
{
	TArray<FGridMapValidationIssue> Issues = Validate(Room);
	const bool bHasError = Issues.ContainsByPredicate([](const FGridMapValidationIssue& Issue)
	{
		return Issue.Severity == EGridMapValidationSeverity::Error;
	});
	if (OutIssues)
	{
		*OutIssues = MoveTemp(Issues);
	}
	return Room.Status == EGridMapRoomStatus::Ready && !bHasError;
}
