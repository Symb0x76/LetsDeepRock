// 网格房间模板资产

#include "GridMapRoomAsset.h"

void UGridMapRoomAsset::InitializeRoom(const int32 Width, const int32 Height, const EGridTileType InitialTile)
{
	Size = FIntPoint(Width, Height);
	Tiles.Init(InitialTile, Width > 0 && Height > 0 ? Width * Height : 0);
	Exits.Reset();
	Markers.Reset();
}

bool UGridMapRoomAsset::IsInBounds(const FIntPoint Coord) const
{
	return Coord.X >= 0 && Coord.Y >= 0 && Coord.X < Size.X && Coord.Y < Size.Y;
}

int32 UGridMapRoomAsset::CoordToIndex(const FIntPoint Coord) const
{
	return IsInBounds(Coord) ? Coord.Y * Size.X + Coord.X : INDEX_NONE;
}

FIntPoint UGridMapRoomAsset::IndexToCoord(const int32 Index) const
{
	return Size.X > 0 && Index >= 0 && Index < Tiles.Num()
		? FIntPoint(Index % Size.X, Index / Size.X)
		: FIntPoint(INDEX_NONE, INDEX_NONE);
}

EGridTileType UGridMapRoomAsset::GetTile(const FIntPoint Coord) const
{
	const int32 Index = CoordToIndex(Coord);
	return Tiles.IsValidIndex(Index) ? Tiles[Index] : EGridTileType::Rock;
}

bool UGridMapRoomAsset::SetTile(const FIntPoint Coord, const EGridTileType TileType)
{
	const int32 Index = CoordToIndex(Coord);
	if (!Tiles.IsValidIndex(Index))
	{
		return false;
	}
	Tiles[Index] = TileType;
	return true;
}
