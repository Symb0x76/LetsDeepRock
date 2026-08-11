// 网格房间模板编辑器 - 编辑器内部共享类型（纯 C++，不参与 UHT）

#pragma once

#include "CoreMinimal.h"
#include "GridMap/GridMapRoomAsset.h"
#include "GridMap/GridMapTypes.h"

/** 画布当前激活的编辑工具。 */
enum class EGridMapRoomTool : uint8
{
	Brush,		// 笔刷
	RectFill,	// 矩形填充
	FloodFill,	// 区域填充
	Eyedropper,	// 吸管
	Marker,		// 语义标记
	Exit		// 出口
};

/** 笔刷覆盖范围。 */
enum class EGridMapRoomBrushSize : uint8
{
	One,	// 1x1
	Three,	// 3x3
	Five	// 5x5
};

namespace GridMapRoomEditor
{
	/** 笔刷半径，0 表示只影响中心格。 */
	inline int32 BrushRadius(const EGridMapRoomBrushSize Size)
	{
		switch (Size)
		{
		case EGridMapRoomBrushSize::Three: return 1;
		case EGridMapRoomBrushSize::Five: return 2;
		default: return 0;
		}
	}

	inline FLinearColor TileColor(const EGridTileType Tile)
	{
		switch (Tile)
		{
		case EGridTileType::Tree: return FLinearColor(0.176f, 0.451f, 0.243f);
		case EGridTileType::Rock: return FLinearColor(0.404f, 0.412f, 0.451f);
		case EGridTileType::LargeTree: return FLinearColor(0.086f, 0.290f, 0.157f);
		default: return FLinearColor(0.137f, 0.145f, 0.165f);
		}
	}

	inline FText TileLabel(const EGridTileType Tile)
	{
		switch (Tile)
		{
		case EGridTileType::Tree: return NSLOCTEXT("GridMapRoomEditor", "Tile_Tree", "一般树木");
		case EGridTileType::Rock: return NSLOCTEXT("GridMapRoomEditor", "Tile_Rock", "岩石");
		case EGridTileType::LargeTree: return NSLOCTEXT("GridMapRoomEditor", "Tile_LargeTree", "高大树木");
		default: return NSLOCTEXT("GridMapRoomEditor", "Tile_Empty", "空地");
		}
	}

	inline FLinearColor MarkerColor(const EGridMapMarkerType Marker)
	{
		switch (Marker)
		{
		case EGridMapMarkerType::PlayerSpawn: return FLinearColor(0.157f, 0.612f, 1.0f);
		case EGridMapMarkerType::Objective: return FLinearColor(1.0f, 0.776f, 0.157f);
		case EGridMapMarkerType::EnemySpawn: return FLinearColor(0.941f, 0.259f, 0.220f);
		default: return FLinearColor(0.694f, 0.447f, 0.898f);
		}
	}

	inline FText MarkerLabel(const EGridMapMarkerType Marker)
	{
		switch (Marker)
		{
		case EGridMapMarkerType::PlayerSpawn: return NSLOCTEXT("GridMapRoomEditor", "Marker_PlayerSpawn", "玩家出生点");
		case EGridMapMarkerType::Objective: return NSLOCTEXT("GridMapRoomEditor", "Marker_Objective", "目标点");
		case EGridMapMarkerType::EnemySpawn: return NSLOCTEXT("GridMapRoomEditor", "Marker_EnemySpawn", "敌人出生点");
		default: return NSLOCTEXT("GridMapRoomEditor", "Marker_Decoration", "装饰点");
		}
	}

	/** 画布上用于快速识别标记的单字符。 */
	inline FString MarkerGlyph(const EGridMapMarkerType Marker)
	{
		switch (Marker)
		{
		case EGridMapMarkerType::PlayerSpawn: return TEXT("P");
		case EGridMapMarkerType::Objective: return TEXT("O");
		case EGridMapMarkerType::EnemySpawn: return TEXT("E");
		default: return TEXT("D");
		}
	}

	inline FText ExitDirectionLabel(const EGridMapExitDirection Direction)
	{
		switch (Direction)
		{
		case EGridMapExitDirection::North: return NSLOCTEXT("GridMapRoomEditor", "Dir_North", "北");
		case EGridMapExitDirection::East: return NSLOCTEXT("GridMapRoomEditor", "Dir_East", "东");
		case EGridMapExitDirection::South: return NSLOCTEXT("GridMapRoomEditor", "Dir_South", "南");
		default: return NSLOCTEXT("GridMapRoomEditor", "Dir_West", "西");
		}
	}

	/** 出口沿边缘延伸方向上的单步偏移。 */
	inline FIntPoint ExitSpanStep(const EGridMapExitDirection Direction)
	{
		return Direction == EGridMapExitDirection::North || Direction == EGridMapExitDirection::South
			? FIntPoint(1, 0)
			: FIntPoint(0, 1);
	}

	inline FLinearColor ExitColor()
	{
		return FLinearColor(0.208f, 0.847f, 0.847f);
	}
}
