// 网格地图阶段三 - 房间图生成器

#include "GridMapGenerator_RoomGraph.h"
#include "GridMapData.h"
#include "GridMapRoomValidator.h"

namespace GridMapRoomGraph
{
struct FTemplateEntry { FString Path; UGridMapRoomAsset* Room = nullptr; };

void HashByte(uint64& Hash, const uint8 Value) { Hash ^= Value; Hash *= 1099511628211ull; }
void HashInt(uint64& Hash, const int64 Value)
{
	for (int32 Shift = 0; Shift < 64; Shift += 8) { HashByte(Hash, static_cast<uint8>(static_cast<uint64>(Value) >> Shift)); }
}
void HashString(uint64& Hash, const FString& Value)
{
	FTCHARToUTF8 Utf8(*Value); HashInt(Hash, Utf8.Length());
	for (int32 Index = 0; Index < Utf8.Length(); ++Index) { HashByte(Hash, static_cast<uint8>(Utf8.Get()[Index])); }
}
FIntPoint ExitCenter(const FGridMapRoomExit& Exit)
{
	return Exit.Start + ((Exit.Direction == EGridMapExitDirection::North || Exit.Direction == EGridMapExitDirection::South)
		? FIntPoint((Exit.Width - 1) / 2, 0) : FIntPoint(0, (Exit.Width - 1) / 2));
}
bool HasMarker(const UGridMapRoomAsset& Room, const EGridMapMarkerType Type)
{
	return Room.Markers.ContainsByPredicate([Type](const FGridMapRoomMarker& Marker) { return Marker.Type == Type; });
}
bool Opposite(const EGridMapExitDirection A, const EGridMapExitDirection B)
{
	return (A == EGridMapExitDirection::North && B == EGridMapExitDirection::South)
		|| (A == EGridMapExitDirection::South && B == EGridMapExitDirection::North)
		|| (A == EGridMapExitDirection::East && B == EGridMapExitDirection::West)
		|| (A == EGridMapExitDirection::West && B == EGridMapExitDirection::East);
}
FIntPoint OutwardStep(const EGridMapExitDirection Direction)
{
	switch (Direction)
	{
	case EGridMapExitDirection::North: return {0, 1};
	case EGridMapExitDirection::East: return {1, 0};
	case EGridMapExitDirection::South: return {0, -1};
	case EGridMapExitDirection::West: return {-1, 0};
	default: return FIntPoint::ZeroValue;
	}
}
bool IsExitCell(const FIntPoint Cell, const FIntPoint Origin, const FGridMapRoomExit& Exit)
{
	for (int32 Offset = 0; Offset < Exit.Width; ++Offset)
	{
		const FIntPoint Local = Exit.Start + ((Exit.Direction == EGridMapExitDirection::North || Exit.Direction == EGridMapExitDirection::South)
			? FIntPoint(Offset, 0) : FIntPoint(0, Offset));
		if (Cell == Origin + Local) { return true; }
	}
	return false;
}
void CollectTemplates(const UGridMapGenerator_RoomGraph& Generator, TArray<FTemplateEntry>& Out)
{
	for (const TSoftObjectPtr<UGridMapRoomAsset>& Soft : Generator.RoomTemplates)
	{
		if (UGridMapRoomAsset* Room = Soft.LoadSynchronous()) { Out.Add({ Soft.ToSoftObjectPath().ToString(), Room }); }
	}
	for (UGridMapRoomAsset* Room : Generator.TransientTemplates)
	{
		if (Room) { Out.Add({ FSoftObjectPath(Room).ToString(), Room }); }
	}
	Out.Sort([](const FTemplateEntry& A, const FTemplateEntry& B) { return A.Path < B.Path; });
}
UGridMapRoomAsset* ResolveRoom(const FSoftObjectPath& Path, const TArray<FTemplateEntry>& Templates)
{
	for (const FTemplateEntry& Entry : Templates) { if (Entry.Path == Path.ToString()) { return Entry.Room; } }
	return Cast<UGridMapRoomAsset>(Path.TryLoad());
}
UGridMapRoomAsset* Pick(FRandomStream& Random, const TArray<FTemplateEntry>& Templates,
	const EGridMapRoomType Type, const bool bNeedsSpawn)
{
	TArray<UGridMapRoomAsset*> Candidates; float TotalWeight = 0.0f;
	for (const FTemplateEntry& Entry : Templates)
	{
		UGridMapRoomAsset* Room = Entry.Room;
		const bool bSpawnMarker = HasMarker(*Room, EGridMapMarkerType::PlayerSpawn);
		if (Room->RoomType == Type && (bNeedsSpawn ? bSpawnMarker : !bSpawnMarker)
			&& FMath::IsFinite(Room->Weight) && Room->Weight > 0.0f && FGridMapRoomValidator::IsValidForGeneration(*Room))
		{
			Candidates.Add(Room); TotalWeight += Room->Weight;
		}
	}
	if (Candidates.IsEmpty() || !FMath::IsFinite(TotalWeight)) { return nullptr; }
	float Roll = Random.FRandRange(0.0f, TotalWeight);
	for (UGridMapRoomAsset* Room : Candidates) { Roll -= Room->Weight; if (Roll <= 0.0f) { return Room; } }
	return Candidates.Last();
}
const FGridMapRoomExit* FindExit(const UGridMapRoomAsset& Room, const EGridMapExitDirection Direction)
{
	return Room.Exits.FindByPredicate([Direction](const FGridMapRoomExit& Exit) { return Exit.Direction == Direction; });
}
bool PointInRect(const FIntPoint Point, const FIntRect& Rect)
{
	return Point.X >= Rect.Min.X && Point.X < Rect.Max.X && Point.Y >= Rect.Min.Y && Point.Y < Rect.Max.Y;
}
}

uint64 UGridMapGenerator_RoomGraph::ComputeTemplateContentHash(const UGridMapRoomAsset& Room)
{
	using namespace GridMapRoomGraph;
	uint64 Hash = 14695981039346656037ull;
	HashInt(Hash, Room.TemplateVersion); HashByte(Hash, static_cast<uint8>(Room.RoomType));
	HashInt(Hash, Room.Size.X); HashInt(Hash, Room.Size.Y); HashInt(Hash, FMath::RoundToInt64(Room.TileSize * 1000.0));
	HashInt(Hash, Room.Tiles.Num()); for (const EGridTileType Tile : Room.Tiles) { HashByte(Hash, static_cast<uint8>(Tile)); }
	TArray<FGridMapRoomExit> Exits = Room.Exits;
	Exits.Sort([](const FGridMapRoomExit& A, const FGridMapRoomExit& B) { return A.Id.LexicalLess(B.Id); });
	HashInt(Hash, Exits.Num());
	for (const FGridMapRoomExit& Exit : Exits)
	{
		HashString(Hash, Exit.Id.ToString()); HashInt(Hash, Exit.Start.X); HashInt(Hash, Exit.Start.Y); HashInt(Hash, Exit.Width);
		HashByte(Hash, static_cast<uint8>(Exit.Direction)); HashString(Hash, Exit.ConnectionTag.ToString());
	}
	TArray<FGridMapRoomMarker> Markers = Room.Markers;
	Markers.Sort([](const FGridMapRoomMarker& A, const FGridMapRoomMarker& B)
	{
		if (A.Type != B.Type) { return A.Type < B.Type; } if (A.GridCoord != B.GridCoord) { return A.GridCoord.X != B.GridCoord.X ? A.GridCoord.X < B.GridCoord.X : A.GridCoord.Y < B.GridCoord.Y; }
		if (A.Yaw != B.Yaw) { return A.Yaw < B.Yaw; } if (A.Group != B.Group) { return A.Group.LexicalLess(B.Group); }
		if (A.Weight != B.Weight) { return A.Weight < B.Weight; } return A.Radius < B.Radius;
	});
	HashInt(Hash, Markers.Num());
	for (const FGridMapRoomMarker& Marker : Markers)
	{
		HashByte(Hash, static_cast<uint8>(Marker.Type)); HashInt(Hash, Marker.GridCoord.X); HashInt(Hash, Marker.GridCoord.Y);
		HashInt(Hash, FMath::RoundToInt64(Marker.Yaw * 1000.0)); HashString(Hash, Marker.Group.ToString());
		HashInt(Hash, FMath::RoundToInt64(Marker.Weight * 1000.0)); HashInt(Hash, FMath::RoundToInt64(Marker.Radius * 1000.0));
	}
	return Hash;
}

bool UGridMapGenerator_RoomGraph::GenerateChecked(UGridMapData* MapData)
{
	LastError.Reset(); GeneratedRecipe = FGridMapGenerationRecipe(); LastRoomBounds.Reset(); bLastTopologyReachable = false;
	if (!MapData || EffectiveSeed == 0) { LastError = TEXT("RoomGraph 需要有效地图和非零 EffectiveSeed"); return false; }
	if (AlgorithmVersion != 1 && AlgorithmVersion != 2) { LastError = TEXT("RoomGraph 仅支持算法版本 1 和 2"); return false; }
	if (CorridorWidth < 1 || CorridorWidth > 5 || CorridorWidth % 2 == 0) { LastError = TEXT("RoomGraph CorridorWidth 仅允许 1、3、5"); return false; }
	auto ResetMap = [this, MapData]()
	{
		MapData->Fill(AlgorithmVersion == 1 ? EGridTileType::Rock : EGridTileType::Uncarved);
	};
	ResetMap();
	if (!ReplayRecipe.IsEmpty())
	{
		if (Replay(MapData, ReplayRecipe)) { return true; } ResetMap(); GeneratedRecipe = FGridMapGenerationRecipe(); return false;
	}
	for (int32 Attempt = 0; Attempt < WholeMapRetries; ++Attempt)
	{
		ResetMap(); FRandomStream Random(EffectiveSeed + Attempt * 7919);
		if (BuildNewRecipe(MapData, Random)) { return true; }
	}
	ResetMap(); GeneratedRecipe = FGridMapGenerationRecipe(); return false;
}

bool UGridMapGenerator_RoomGraph::BuildNewRecipe(UGridMapData* MapData, FRandomStream& Random)
{
	using namespace GridMapRoomGraph;
	TArray<FTemplateEntry> Templates; CollectTemplates(*this, Templates);
	UGridMapRoomAsset* Spawn = Pick(Random, Templates, EGridMapRoomType::Connector, true);
	UGridMapRoomAsset* Combat = Pick(Random, Templates, EGridMapRoomType::Combat, false);
	UGridMapRoomAsset* Objective = Pick(Random, Templates, EGridMapRoomType::Objective, false);
	UGridMapRoomAsset* BranchRoom = Pick(Random, Templates, EGridMapRoomType::Connector, false);
	if (!Spawn || !Combat || !Objective || !BranchRoom) { LastError = TEXT("RoomGraph 缺少有效的出生 Connector、Connector、Combat 或 Objective 模板"); return false; }
	TArray<UGridMapRoomAsset*> Rooms = { Spawn }; TArray<EGridMapRoomPurpose> Purposes = { EGridMapRoomPurpose::Spawn };
	for (int32 Index = 0; Index < ConnectorRoomCount; ++Index)
	{
		UGridMapRoomAsset* Connector = Pick(Random, Templates, EGridMapRoomType::Connector, false);
		if (!Connector) { LastError = TEXT("RoomGraph 缺少普通 Connector 模板"); return false; }
		Rooms.Add(Connector); Purposes.Add(EGridMapRoomPurpose::Connector);
	}
	Rooms.Add(Combat); Purposes.Add(EGridMapRoomPurpose::Combat); Rooms.Add(Objective); Purposes.Add(EGridMapRoomPurpose::Objective);
	Rooms.Add(BranchRoom); Purposes.Add(EGridMapRoomPurpose::Branch);
	GeneratedRecipe = FGridMapGenerationRecipe(); LastRoomBounds.Reset();
	const int32 MainY = Random.RandRange(2, 6); int32 X = Random.RandRange(2, 5);
	for (int32 Index = 0; Index < Rooms.Num() - 1; ++Index)
	{
		UGridMapRoomAsset* Room = Rooms[Index]; const FIntPoint Origin(X, MainY); const FIntRect Bounds(Origin, Origin + Room->Size);
		if (Bounds.Max.X > MapData->GetConfig().MapWidth || Bounds.Max.Y > MapData->GetConfig().MapHeight) { LastError = TEXT("RoomGraph 主链越界"); return false; }
		LastRoomBounds.Add(Bounds); FGridMapRecipeRoom& Saved = GeneratedRecipe.Rooms.AddDefaulted_GetRef();
		Saved.TemplatePath = FSoftObjectPath(Room); Saved.TemplateVersion = Room->TemplateVersion; Saved.TemplateContentHash = ComputeTemplateContentHash(*Room);
		Saved.Origin = Origin; Saved.Size = Room->Size; Saved.Purpose = Purposes[Index]; X += Room->Size.X + Random.RandRange(3, 6);
	}
	const int32 AttachIndex = Random.RandRange(0, ConnectorRoomCount);
	const int32 BranchIndex = Rooms.Num() - 1; const int32 BranchGap = Random.RandRange(3, 6);
	const FGridMapRoomExit* AttachExit = FindExit(*Rooms[AttachIndex], EGridMapExitDirection::North);
	const FGridMapRoomExit* BranchExit = FindExit(*BranchRoom, EGridMapExitDirection::South);
	if (!AttachExit || !BranchExit) { LastError = TEXT("RoomGraph 支路缺少 North/South 出口"); return false; }
	const int32 BranchX = GeneratedRecipe.Rooms[AttachIndex].Origin.X + ExitCenter(*AttachExit).X - ExitCenter(*BranchExit).X;
	int32 MainTop = 0; for (const FGridMapRecipeRoom& Room : GeneratedRecipe.Rooms) { MainTop = FMath::Max(MainTop, Room.Origin.Y + Room.Size.Y); }
	const FIntPoint BranchOrigin(BranchX, MainTop + BranchGap); const FIntRect BranchBounds(BranchOrigin, BranchOrigin + BranchRoom->Size);
	if (BranchBounds.Min.X < 0 || BranchBounds.Max.X > MapData->GetConfig().MapWidth || BranchBounds.Max.Y > MapData->GetConfig().MapHeight) { LastError = TEXT("RoomGraph 支路越界"); return false; }
	for (const FIntRect& Existing : LastRoomBounds) { if (BranchBounds.Intersect(Existing)) { LastError = TEXT("RoomGraph 支路重叠"); return false; } }
	LastRoomBounds.Add(BranchBounds); FGridMapRecipeRoom& BranchSaved = GeneratedRecipe.Rooms.AddDefaulted_GetRef();
	BranchSaved.TemplatePath = FSoftObjectPath(BranchRoom); BranchSaved.TemplateVersion = BranchRoom->TemplateVersion;
	BranchSaved.TemplateContentHash = ComputeTemplateContentHash(*BranchRoom); BranchSaved.Origin = BranchOrigin; BranchSaved.Size = BranchRoom->Size; BranchSaved.Purpose = EGridMapRoomPurpose::Branch;
	for (int32 Index = 0; Index < Rooms.Num() - 2; ++Index)
	{
		const FGridMapRoomExit* From = FindExit(*Rooms[Index], EGridMapExitDirection::East); const FGridMapRoomExit* To = FindExit(*Rooms[Index + 1], EGridMapExitDirection::West);
		if (!From || !To) { LastError = TEXT("RoomGraph 主链缺少 East/West 出口"); return false; }
		FGridMapRecipeConnection& C = GeneratedRecipe.Connections.AddDefaulted_GetRef(); C.FromRoom = Index; C.ToRoom = Index + 1; C.CorridorWidth = CorridorWidth;
		const FIntPoint A = GeneratedRecipe.Rooms[Index].Origin + ExitCenter(*From); const FIntPoint B = GeneratedRecipe.Rooms[Index + 1].Origin + ExitCenter(*To);
		const FIntPoint AOutside = A + OutwardStep(From->Direction); const FIntPoint BOutside = B + OutwardStep(To->Direction);
		C.Points = Random.RandRange(0, 1) == 0 ? TArray<FIntPoint>{ A, AOutside, {BOutside.X, AOutside.Y}, BOutside, B }
			: TArray<FIntPoint>{ A, AOutside, {AOutside.X, BOutside.Y}, BOutside, B };
	}
	FGridMapRecipeConnection& Branch = GeneratedRecipe.Connections.AddDefaulted_GetRef(); Branch.FromRoom = AttachIndex; Branch.ToRoom = BranchIndex; Branch.CorridorWidth = CorridorWidth;
	const FIntPoint A = GeneratedRecipe.Rooms[AttachIndex].Origin + ExitCenter(*AttachExit); const FIntPoint B = BranchOrigin + ExitCenter(*BranchExit);
	Branch.Points = { A, A + OutwardStep(AttachExit->Direction), B + OutwardStep(BranchExit->Direction), B };
	return Replay(MapData, GeneratedRecipe);
}

bool UGridMapGenerator_RoomGraph::Replay(UGridMapData* MapData, const FGridMapGenerationRecipe& Recipe)
{
	using namespace GridMapRoomGraph;
	TArray<FTemplateEntry> Templates; CollectTemplates(*this, Templates); LastRoomBounds.Reset(); GeneratedRecipe = Recipe; GeneratedRecipe.Markers.Reset();
	TArray<UGridMapRoomAsset*> Rooms; int32 SpawnPurpose = 0, ObjectivePurpose = 0, CombatPurpose = 0, BranchPurpose = 0, SpawnMarkers = 0; int32 SpawnIndex = INDEX_NONE;
	for (int32 Index = 0; Index < Recipe.Rooms.Num(); ++Index)
	{
		const FGridMapRecipeRoom& Saved = Recipe.Rooms[Index]; UGridMapRoomAsset* Room = ResolveRoom(Saved.TemplatePath, Templates);
		if (!Room || Saved.TemplateVersion <= 0 || Saved.TemplateContentHash == 0 || Room->TemplateVersion != Saved.TemplateVersion
			|| ComputeTemplateContentHash(*Room) != Saved.TemplateContentHash || Room->Size != Saved.Size) { LastError = TEXT("RoomGraph Recipe 模板缺失、版本或内容哈希不一致"); return false; }
		const bool bHasSpawn = HasMarker(*Room, EGridMapMarkerType::PlayerSpawn); const bool bHasObjective = HasMarker(*Room, EGridMapMarkerType::Objective);
		const bool bPurposeValid = (Saved.Purpose == EGridMapRoomPurpose::Spawn && Room->RoomType == EGridMapRoomType::Connector && bHasSpawn)
			|| ((Saved.Purpose == EGridMapRoomPurpose::Connector || Saved.Purpose == EGridMapRoomPurpose::Branch) && Room->RoomType == EGridMapRoomType::Connector && !bHasSpawn)
			|| (Saved.Purpose == EGridMapRoomPurpose::Combat && Room->RoomType == EGridMapRoomType::Combat)
			|| (Saved.Purpose == EGridMapRoomPurpose::Objective && Room->RoomType == EGridMapRoomType::Objective && bHasObjective);
		if (!bPurposeValid) { LastError = TEXT("RoomGraph Recipe Purpose 与模板语义不匹配"); return false; }
		SpawnPurpose += Saved.Purpose == EGridMapRoomPurpose::Spawn; ObjectivePurpose += Saved.Purpose == EGridMapRoomPurpose::Objective;
		CombatPurpose += Saved.Purpose == EGridMapRoomPurpose::Combat; BranchPurpose += Saved.Purpose == EGridMapRoomPurpose::Branch; if (Saved.Purpose == EGridMapRoomPurpose::Spawn) { SpawnIndex = Index; }
		const FIntRect Bounds(Saved.Origin, Saved.Origin + Saved.Size);
		if (Bounds.Min.X < 0 || Bounds.Min.Y < 0 || Bounds.Max.X > MapData->GetConfig().MapWidth || Bounds.Max.Y > MapData->GetConfig().MapHeight) { LastError = TEXT("RoomGraph Recipe 房间越界"); return false; }
		for (const FIntRect& Existing : LastRoomBounds) { if (Bounds.Intersect(Existing)) { LastError = TEXT("RoomGraph Recipe 房间重叠"); return false; } }
		LastRoomBounds.Add(Bounds); Rooms.Add(Room);
		for (const FGridMapRoomMarker& Marker : Room->Markers) { SpawnMarkers += Marker.Type == EGridMapMarkerType::PlayerSpawn; FGridMapRecipeMarker& Global = GeneratedRecipe.Markers.AddDefaulted_GetRef(); Global.Type = static_cast<uint8>(Marker.Type); Global.GridCoord = Saved.Origin + Marker.GridCoord; }
	}
	if (SpawnPurpose != 1 || ObjectivePurpose != 1 || CombatPurpose < 1 || BranchPurpose < 1 || SpawnMarkers != 1) { LastError = TEXT("RoomGraph Recipe 必需 Purpose 或唯一出生点不满足"); return false; }
	TArray<TArray<int32>> Graph; Graph.SetNum(Rooms.Num());
	for (const FGridMapRecipeConnection& C : Recipe.Connections)
	{
		if (!Rooms.IsValidIndex(C.FromRoom) || !Rooms.IsValidIndex(C.ToRoom) || C.FromRoom == C.ToRoom
			|| C.CorridorWidth < 1 || C.CorridorWidth > 5 || C.CorridorWidth % 2 == 0 || C.Points.Num() < 4) { LastError = TEXT("RoomGraph Recipe Connection 基本结构无效"); return false; }
		const FGridMapRoomExit* FromExit = Rooms[C.FromRoom]->Exits.FindByPredicate([&](const FGridMapRoomExit& E) { return LastRoomBounds[C.FromRoom].Min + ExitCenter(E) == C.Points[0]; });
		const FGridMapRoomExit* ToExit = Rooms[C.ToRoom]->Exits.FindByPredicate([&](const FGridMapRoomExit& E) { return LastRoomBounds[C.ToRoom].Min + ExitCenter(E) == C.Points.Last(); });
		if (!FromExit || !ToExit || !Opposite(FromExit->Direction, ToExit->Direction)) { LastError = TEXT("RoomGraph Recipe Connection 端点不是方向相反的真实出口"); return false; }
		if (C.Points[1] != C.Points[0] + OutwardStep(FromExit->Direction)
			|| C.Points[C.Points.Num() - 2] != C.Points.Last() + OutwardStep(ToExit->Direction))
		{
			LastError = TEXT("RoomGraph Recipe 走廊未先离开出口或未从外侧进入目标出口"); return false;
		}
		for (int32 Segment = 1; Segment < C.Points.Num(); ++Segment)
		{
			const FIntPoint P0 = C.Points[Segment - 1], P1 = C.Points[Segment]; if (P0.X != P1.X && P0.Y != P1.Y) { LastError = TEXT("RoomGraph Recipe 走廊非轴对齐"); return false; }
			const FIntPoint Step(FMath::Sign(P1.X - P0.X), FMath::Sign(P1.Y - P0.Y)); FIntPoint P = P0;
			for (;; P += Step)
			{
				for (int32 Offset = -(C.CorridorWidth / 2); Offset <= C.CorridorWidth / 2; ++Offset)
				{
					const FIntPoint Cell(P.X + (Step.Y != 0 ? Offset : 0), P.Y + (Step.X != 0 ? Offset : 0));
					if (!MapData->IsValidCoord(Cell.X, Cell.Y)) { LastError = TEXT("RoomGraph Recipe 走廊越界"); return false; }
					for (int32 RoomIndex = 0; RoomIndex < LastRoomBounds.Num(); ++RoomIndex)
					{
						if (!PointInRect(Cell, LastRoomBounds[RoomIndex])) { continue; }
						const bool bAllowedFrom = RoomIndex == C.FromRoom && IsExitCell(Cell, LastRoomBounds[RoomIndex].Min, *FromExit);
						const bool bAllowedTo = RoomIndex == C.ToRoom && IsExitCell(Cell, LastRoomBounds[RoomIndex].Min, *ToExit);
						if (!bAllowedFrom && !bAllowedTo) { LastError = TEXT("RoomGraph Recipe 走廊切入房间非出口区域"); return false; }
					}
				}
				if (P == P1) { break; }
			}
		}
		Graph[C.FromRoom].Add(C.ToRoom); Graph[C.ToRoom].Add(C.FromRoom);
	}
	TSet<int32> GraphReachable; TArray<int32> GraphQueue = { SpawnIndex }; GraphReachable.Add(SpawnIndex);
	for (int32 Head = 0; Head < GraphQueue.Num(); ++Head) { for (const int32 Next : Graph[GraphQueue[Head]]) { if (!GraphReachable.Contains(Next)) { GraphReachable.Add(Next); GraphQueue.Add(Next); } } }
	if (GraphReachable.Num() != Rooms.Num()) { LastError = TEXT("RoomGraph Recipe 图存在未连接房间"); return false; }
	for (int32 Index = 0; Index < Rooms.Num(); ++Index)
	{
		for (int32 Y = 0; Y < Rooms[Index]->Size.Y; ++Y) { for (int32 X = 0; X < Rooms[Index]->Size.X; ++X) { WriteTile(MapData, LastRoomBounds[Index].Min.X + X, LastRoomBounds[Index].Min.Y + Y, Rooms[Index]->GetTile({X, Y})); } }
	}
	auto ForLine = [](const FGridMapRecipeConnection& C, TFunctionRef<void(FIntPoint)> Visit)
	{
		for (int32 Segment = 1; Segment < C.Points.Num(); ++Segment) { const FIntPoint A = C.Points[Segment - 1], B = C.Points[Segment]; const FIntPoint Step(FMath::Sign(B.X - A.X), FMath::Sign(B.Y - A.Y)); FIntPoint P = A; for (;;) { for (int32 O = -(C.CorridorWidth / 2); O <= C.CorridorWidth / 2; ++O) { Visit({P.X + (Step.Y != 0 ? O : 0), P.Y + (Step.X != 0 ? O : 0)}); } if (P == B) { break; } P += Step; } }
	};
	TSet<FIntPoint> Footprint;
	if (AlgorithmVersion == CurrentAlgorithmVersion)
	{
		for (const FIntRect& Bounds : LastRoomBounds)
		{
			for (int32 Y = Bounds.Min.Y; Y < Bounds.Max.Y; ++Y) { for (int32 X = Bounds.Min.X; X < Bounds.Max.X; ++X) { Footprint.Add({X, Y}); } }
		}
	}
	for (const FGridMapRecipeConnection& C : Recipe.Connections)
	{
		ForLine(C, [this, MapData, &Footprint](const FIntPoint P)
		{
			WriteTile(MapData, P.X, P.Y, EGridTileType::Empty);
			if (AlgorithmVersion == CurrentAlgorithmVersion) { Footprint.Add(P); }
		});
	}
	if (AlgorithmVersion == 2)
	{
		TSet<FIntPoint> BoundaryCells;
		for (const FIntPoint Cell : Footprint)
		{
			for (int32 OffsetY = -1; OffsetY <= 1; ++OffsetY)
			{
				for (int32 OffsetX = -1; OffsetX <= 1; ++OffsetX)
				{
					const FIntPoint NeighborCoord = Cell + FIntPoint(OffsetX, OffsetY);
					const FGridTileData* Neighbor = MapData->GetTile(NeighborCoord.X, NeighborCoord.Y);
					if (!Footprint.Contains(NeighborCoord) && Neighbor && Neighbor->TileType == EGridTileType::Uncarved) { BoundaryCells.Add(NeighborCoord); }
				}
			}
		}
		for (const FIntPoint Cell : BoundaryCells) { WriteTile(MapData, Cell.X, Cell.Y, EGridTileType::Rock); }
	}
	const FGridMapRecipeMarker* Spawn = GeneratedRecipe.Markers.FindByPredicate([](const FGridMapRecipeMarker& M) { return M.Type == static_cast<uint8>(EGridMapMarkerType::PlayerSpawn); });
	if (!Spawn || !MapData->GetTile(Spawn->GridCoord.X, Spawn->GridCoord.Y)->IsPassable()) { LastError = TEXT("RoomGraph BFS 起点不可通行"); return false; }
	TSet<FIntPoint> Reachable; TArray<FIntPoint> Queue = { Spawn->GridCoord }; Reachable.Add(Spawn->GridCoord);
	for (int32 Head = 0; Head < Queue.Num(); ++Head) { static const FIntPoint Steps[] = {{1,0},{-1,0},{0,1},{0,-1}}; for (const FIntPoint Step : Steps) { const FIntPoint Next = Queue[Head] + Step; const FGridTileData* Tile = MapData->GetTile(Next.X, Next.Y); if (Tile && Tile->IsPassable() && !Reachable.Contains(Next)) { Reachable.Add(Next); Queue.Add(Next); } } }
	bLastTopologyReachable = true;
	for (const FGridMapRecipeMarker& Marker : GeneratedRecipe.Markers) { if ((Marker.Type == static_cast<uint8>(EGridMapMarkerType::Objective) || Marker.Type == static_cast<uint8>(EGridMapMarkerType::EnemySpawn)) && !Reachable.Contains(Marker.GridCoord)) { bLastTopologyReachable = false; } }
	for (int32 Index = 0; Index < Rooms.Num(); ++Index) { bool bRepresentative = false; for (int32 Y = LastRoomBounds[Index].Min.Y; Y < LastRoomBounds[Index].Max.Y && !bRepresentative; ++Y) { for (int32 X = LastRoomBounds[Index].Min.X; X < LastRoomBounds[Index].Max.X; ++X) { if (MapData->GetTile(X, Y)->IsPassable() && Reachable.Contains({X, Y})) { bRepresentative = true; break; } } } bLastTopologyReachable &= bRepresentative; }
	for (const FGridMapRecipeConnection& C : Recipe.Connections) { ForLine(C, [&Reachable, this](const FIntPoint P) { bLastTopologyReachable &= Reachable.Contains(P); }); }
	if (!bLastTopologyReachable) { LastError = TEXT("RoomGraph BFS 检测到 Marker、房间代表格或完整走廊不可达"); }
	return bLastTopologyReachable;
}
