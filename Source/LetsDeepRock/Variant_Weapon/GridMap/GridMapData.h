// 网格地图系统 - 数据层

#pragma once

#include "CoreMinimal.h"
#include "GridMapTypes.h"
#include "GridMapData.generated.h"

class AGridMapManager;
class UGridMapGenerator_Default;
class UGridMapGenerator_Simple;
class UGridMapGenerator_Test;

/**
 * UGridMapData
 * 
 * 网格地图数据容器，存储二维网格的所有格子信息。
 * 使用紧凑的一维数组存储，通过坐标计算索引。
 * 
 * 该对象不是 Actor，由 GridMapManager 持有和管理。
 */
UCLASS(BlueprintType)
class LETSDEEPROCK_API UGridMapData : public UObject
{
	GENERATED_BODY()

public:
	/** 根据配置初始化地图数据（全部填充为空地） */
	void Initialize(const FGridMapConfig& InConfig);

	/** 获取地图配置 */
	const FGridMapConfig& GetConfig() const { return Config; }

	/** 获取指定坐标的格子数据（只读） */
	const FGridTileData* GetTile(int32 X, int32 Y) const;

	/** 坐标是否有效 */
	bool IsValidCoord(int32 X, int32 Y) const;

	/** 将网格坐标转换为世界位置（格子中心） */
	FVector GridToWorld(int32 X, int32 Y) const;

	/** 将世界位置转换为网格坐标 */
	bool WorldToGrid(const FVector& WorldLocation, int32& OutX, int32& OutY) const;

	/** 获取所有格子数据的只读引用（用于渲染遍历等） */
	const TArray<FGridTileData>& GetAllTiles() const { return Tiles; }

	/** 获取地图总格子数 */
	int32 GetTotalTiles() const { return Tiles.Num(); }

private:
	friend class AGridMapManager;
	friend class UGridMapGenerator_Default;
	friend class UGridMapGenerator_Simple;
	friend class UGridMapGenerator_Test;

	/** 运行时仅允许 Manager 写入；生成阶段仅允许已登记的生成器写入。 */
	FGridTileData* GetTileMutable(int32 X, int32 Y);
	void SetTileType(int32 X, int32 Y, EGridTileType NewType);

	/** 坐标转一维索引 */
	int32 CoordToIndex(int32 X, int32 Y) const;

	/** 根据类型获取对应耐久值 */
	int32 GetHealthForType(EGridTileType Type) const;

	UPROPERTY()
	FGridMapConfig Config;

	// 紧凑一维数组存储所有格子数据
	UPROPERTY()
	TArray<FGridTileData> Tiles;
};
