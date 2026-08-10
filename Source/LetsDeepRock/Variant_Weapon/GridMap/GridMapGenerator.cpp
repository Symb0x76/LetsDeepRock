// 网格地图系统 - 生成器实现

#include "GridMapGenerator.h"
#include "GridMapData.h"

// ============================================================
// UGridMapGenerator (基类)
// ============================================================

void UGridMapGenerator::Generate(UGridMapData* MapData)
{
	// 基类默认实现：什么都不做，子类必须重写
	UE_LOG(LogTemp, Warning, TEXT("[GridMap] UGridMapGenerator::Generate 基类被调用，请使用子类。"));
}

bool UGridMapGenerator::GenerateChecked(UGridMapData* MapData)
{
	LastError.Reset();
	Generate(MapData);
	return MapData != nullptr;
}

void UGridMapGenerator::WriteTile(UGridMapData* MapData, const int32 X, const int32 Y, const EGridTileType Type) const
{
	if (MapData)
	{
		MapData->SetTileType(X, Y, Type);
	}
}

namespace
{
void HashByte(uint64& Hash, const uint8 Value)
{
	Hash ^= Value;
	Hash *= 1099511628211ull;
}

void HashInt(uint64& Hash, const int64 Value)
{
	for (int32 Shift = 0; Shift < 64; Shift += 8)
	{
		HashByte(Hash, static_cast<uint8>((static_cast<uint64>(Value) >> Shift) & 0xff));
	}
}

void HashString(uint64& Hash, const FString& Value)
{
	FTCHARToUTF8 Utf8(*Value);
	HashInt(Hash, Utf8.Length());
	for (int32 Index = 0; Index < Utf8.Length(); ++Index)
	{
		HashByte(Hash, static_cast<uint8>(Utf8.Get()[Index]));
	}
}
}

uint64 UGridMapGenerator::ComputeStableResultHash(const UGridMapData& MapData, const int32 Seed,
	const int32 AlgorithmVersion, const int32 TemplateSetVersion, const FGridMapGenerationRecipe& Recipe)
{
	uint64 Hash = 14695981039346656037ull;
	HashInt(Hash, Seed);
	HashInt(Hash, AlgorithmVersion);
	HashInt(Hash, TemplateSetVersion);
	const FGridMapConfig& Config = MapData.GetConfig();
	HashInt(Hash, Config.MapWidth); HashInt(Hash, Config.MapHeight);
	HashInt(Hash, FMath::RoundToInt64(Config.TileSize * 1000.0));
	HashInt(Hash, Config.TreeHealth); HashInt(Hash, Config.LargeTreeHealth);
	HashInt(Hash, Recipe.Rooms.Num());
	for (const FGridMapRecipeRoom& Room : Recipe.Rooms)
	{
		HashString(Hash, Room.TemplatePath.ToString());
		HashInt(Hash, Room.TemplateVersion);
		HashInt(Hash, Room.TemplateContentHash);
		HashInt(Hash, Room.Origin.X); HashInt(Hash, Room.Origin.Y);
		HashInt(Hash, Room.Size.X); HashInt(Hash, Room.Size.Y);
		HashByte(Hash, static_cast<uint8>(Room.Purpose));
	}
	HashInt(Hash, Recipe.Connections.Num());
	for (const FGridMapRecipeConnection& Connection : Recipe.Connections)
	{
		HashInt(Hash, Connection.FromRoom); HashInt(Hash, Connection.ToRoom);
		HashInt(Hash, Connection.CorridorWidth); HashInt(Hash, Connection.Points.Num());
		for (const FIntPoint Point : Connection.Points)
		{
			HashInt(Hash, Point.X); HashInt(Hash, Point.Y);
		}
	}
	HashInt(Hash, Recipe.Markers.Num());
	for (const FGridMapRecipeMarker& Marker : Recipe.Markers)
	{
		HashByte(Hash, Marker.Type); HashInt(Hash, Marker.GridCoord.X); HashInt(Hash, Marker.GridCoord.Y);
	}
	for (const FGridTileData& Tile : MapData.GetAllTiles())
	{
		HashByte(Hash, static_cast<uint8>(Tile.TileType));
		HashInt(Hash, Tile.Health); HashInt(Hash, Tile.MaxHealth);
	}
	return Hash;
}

// ============================================================
// UGridMapGenerator_Default
// ============================================================

void UGridMapGenerator_Default::Generate(UGridMapData* MapData)
{
	if (!MapData)
	{
		return;
	}

	const FGridMapConfig& Config = MapData->GetConfig();
	const int32 Width = Config.MapWidth;
	const int32 Height = Config.MapHeight;

	// 四周边界设为岩石
	for (int32 X = 0; X < Width; ++X)
	{
		WriteTile(MapData, X, 0, EGridTileType::Rock);            // 下边界
		WriteTile(MapData, X, Height - 1, EGridTileType::Rock);   // 上边界
	}

	for (int32 Y = 0; Y < Height; ++Y)
	{
		WriteTile(MapData, 0, Y, EGridTileType::Rock);            // 左边界
		WriteTile(MapData, Width - 1, Y, EGridTileType::Rock);    // 右边界
	}

	UE_LOG(LogTemp, Log, TEXT("[GridMap] Default 生成完成：%dx%d，四周岩石边界。"), Width, Height);
}

// ============================================================
// UGridMapGenerator_Simple
// ============================================================

void UGridMapGenerator_Simple::Generate(UGridMapData* MapData)
{
	if (!MapData)
	{
		return;
	}

	const FGridMapConfig& Config = MapData->GetConfig();
	const int32 Width = Config.MapWidth;
	const int32 Height = Config.MapHeight;

	// 确定随机流
	FRandomStream RandomStream(EffectiveSeed);

	// 先做边界岩石
	for (int32 X = 0; X < Width; ++X)
	{
		WriteTile(MapData, X, 0, EGridTileType::Rock);
		WriteTile(MapData, X, Height - 1, EGridTileType::Rock);
	}
	for (int32 Y = 0; Y < Height; ++Y)
	{
		WriteTile(MapData, 0, Y, EGridTileType::Rock);
		WriteTile(MapData, Width - 1, Y, EGridTileType::Rock);
	}

	// 内部随机散布树木
	int32 TreeCount = 0;
	int32 LargeTreeCount = 0;

	for (int32 Y = 1; Y < Height - 1; ++Y)
	{
		for (int32 X = 1; X < Width - 1; ++X)
		{
			const float Roll = RandomStream.FRand();

			if (Roll < LargeTreeDensity)
			{
				WriteTile(MapData, X, Y, EGridTileType::LargeTree);
				++LargeTreeCount;
			}
			else if (Roll < LargeTreeDensity + TreeDensity)
			{
				WriteTile(MapData, X, Y, EGridTileType::Tree);
				++TreeCount;
			}
			// 否则保持空地
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[GridMap] Simple 生成完成：%dx%d，树木=%d，高大树木=%d。"),
		Width, Height, TreeCount, LargeTreeCount);
}

// ============================================================
// UGridMapGenerator_Test
// ============================================================

void UGridMapGenerator_Test::Generate(UGridMapData* MapData)
{
	if (!MapData)
	{
		return;
	}

	const FGridMapConfig& Config = MapData->GetConfig();
	const int32 Width = Config.MapWidth;
	const int32 Height = Config.MapHeight;

	// 在地图中央区域放置三种类型各一个，间隔 3 格方便观察
	const int32 CenterX = Width / 2;
	const int32 CenterY = Height / 2;

	// 从左到右：Tree、Rock、LargeTree
	WriteTile(MapData, CenterX - 3, CenterY, EGridTileType::Tree);
	WriteTile(MapData, CenterX, CenterY, EGridTileType::Rock);
	WriteTile(MapData, CenterX + 3, CenterY, EGridTileType::LargeTree);

	UE_LOG(LogTemp, Log, TEXT("[GridMap] Test 生成完成：%dx%d，中央放置 Tree(%d,%d) Rock(%d,%d) LargeTree(%d,%d)。"),
		Width, Height,
		CenterX - 3, CenterY,
		CenterX, CenterY,
		CenterX + 3, CenterY);
}
