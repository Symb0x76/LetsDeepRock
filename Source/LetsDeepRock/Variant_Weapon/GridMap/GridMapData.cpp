// 网格地图系统 - 数据层实现

#include "GridMapData.h"

void UGridMapData::Initialize(const FGridMapConfig& InConfig)
{
	Config = InConfig;

	const int32 TotalTiles = Config.MapWidth * Config.MapHeight;
	Tiles.SetNum(TotalTiles);

	// 初始化所有格子为空地
	for (int32 i = 0; i < TotalTiles; ++i)
	{
		const int32 X = i % Config.MapWidth;
		const int32 Y = i / Config.MapWidth;

		Tiles[i].TileType = EGridTileType::Empty;
		Tiles[i].Health = 0;
		Tiles[i].MaxHealth = 0;
		Tiles[i].GridX = X;
		Tiles[i].GridY = Y;
	}
}

void UGridMapData::Fill(const EGridTileType Type)
{
	const int32 Health = GetHealthForType(Type);
	for (FGridTileData& Tile : Tiles)
	{
		Tile.TileType = Type;
		Tile.Health = Health;
		Tile.MaxHealth = Health;
	}
}

const FGridTileData* UGridMapData::GetTile(int32 X, int32 Y) const
{
	if (!IsValidCoord(X, Y))
	{
		return nullptr;
	}
	return &Tiles[CoordToIndex(X, Y)];
}

FGridTileData* UGridMapData::GetTileMutable(int32 X, int32 Y)
{
	if (!IsValidCoord(X, Y))
	{
		return nullptr;
	}
	return &Tiles[CoordToIndex(X, Y)];
}

void UGridMapData::SetTileType(int32 X, int32 Y, EGridTileType NewType)
{
	FGridTileData* Tile = GetTileMutable(X, Y);
	if (!Tile)
	{
		return;
	}

	Tile->TileType = NewType;
	Tile->MaxHealth = GetHealthForType(NewType);
	Tile->Health = Tile->MaxHealth;
}

bool UGridMapData::IsValidCoord(int32 X, int32 Y) const
{
	return X >= 0 && X < Config.MapWidth && Y >= 0 && Y < Config.MapHeight;
}

FVector UGridMapData::GridToWorld(int32 X, int32 Y) const
{
	// 格子中心位置，地图原点在 (0,0) 格的左下角
	const float WorldX = (static_cast<float>(X) + 0.5f) * Config.TileSize;
	const float WorldY = (static_cast<float>(Y) + 0.5f) * Config.TileSize;
	return FVector(WorldX, WorldY, 0.0f);
}

bool UGridMapData::WorldToGrid(const FVector& WorldLocation, int32& OutX, int32& OutY) const
{
	OutX = FMath::FloorToInt(WorldLocation.X / Config.TileSize);
	OutY = FMath::FloorToInt(WorldLocation.Y / Config.TileSize);
	return IsValidCoord(OutX, OutY);
}

int32 UGridMapData::CoordToIndex(int32 X, int32 Y) const
{
	return Y * Config.MapWidth + X;
}

int32 UGridMapData::GetHealthForType(EGridTileType Type) const
{
	switch (Type)
	{
	case EGridTileType::Tree:
		return Config.TreeHealth;
	case EGridTileType::LargeTree:
		return Config.LargeTreeHealth;
	default:
		return 0; // 空地和岩石不需要耐久值
	}
}
