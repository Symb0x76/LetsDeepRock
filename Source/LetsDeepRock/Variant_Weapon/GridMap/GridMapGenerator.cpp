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
		MapData->SetTileType(X, 0, EGridTileType::Rock);            // 下边界
		MapData->SetTileType(X, Height - 1, EGridTileType::Rock);   // 上边界
	}

	for (int32 Y = 0; Y < Height; ++Y)
	{
		MapData->SetTileType(0, Y, EGridTileType::Rock);            // 左边界
		MapData->SetTileType(Width - 1, Y, EGridTileType::Rock);    // 右边界
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
	FRandomStream RandomStream;
	if (Seed != 0)
	{
		RandomStream.Initialize(Seed);
	}
	else
	{
		RandomStream.GenerateNewSeed();
	}

	// 先做边界岩石
	for (int32 X = 0; X < Width; ++X)
	{
		MapData->SetTileType(X, 0, EGridTileType::Rock);
		MapData->SetTileType(X, Height - 1, EGridTileType::Rock);
	}
	for (int32 Y = 0; Y < Height; ++Y)
	{
		MapData->SetTileType(0, Y, EGridTileType::Rock);
		MapData->SetTileType(Width - 1, Y, EGridTileType::Rock);
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
				MapData->SetTileType(X, Y, EGridTileType::LargeTree);
				++LargeTreeCount;
			}
			else if (Roll < LargeTreeDensity + TreeDensity)
			{
				MapData->SetTileType(X, Y, EGridTileType::Tree);
				++TreeCount;
			}
			// 否则保持空地
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[GridMap] Simple 生成完成：%dx%d，树木=%d，高大树木=%d。"),
		Width, Height, TreeCount, LargeTreeCount);
}
