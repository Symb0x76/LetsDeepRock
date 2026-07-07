// 网格地图系统 - 类型定义

#pragma once

#include "CoreMinimal.h"
#include "GridMapTypes.generated.h"

/**
 * 地面格子类型枚举
 */
UENUM(BlueprintType)
enum class EGridTileType : uint8
{
	Empty = 0		UMETA(DisplayName = "空地"),
	Tree = 1		UMETA(DisplayName = "一般树木"),
	Rock = 2		UMETA(DisplayName = "岩石"),
	LargeTree = 3	UMETA(DisplayName = "高大树木")
};

/**
 * 单个格子的运行时数据
 */
USTRUCT(BlueprintType)
struct FGridTileData
{
	GENERATED_BODY()

	// 格子类型
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	EGridTileType TileType = EGridTileType::Empty;

	// 当前耐久值（仅对可破坏类型有效）
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 Health = 0;

	// 最大耐久值
	UPROPERTY(EditAnywhere, BlueprintReadWrite)
	int32 MaxHealth = 0;

	// 格子坐标（行列）
	UPROPERTY(BlueprintReadOnly)
	int32 GridX = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 GridY = 0;

	/** 判断该格子是否可通行 */
	bool IsPassable() const
	{
		return TileType == EGridTileType::Empty;
	}

	/** 判断该格子是否可被破坏 */
	bool IsDestructible() const
	{
		return TileType == EGridTileType::Tree || TileType == EGridTileType::LargeTree;
	}
};

/**
 * 网格地图配置（DataAsset 或直接在 Manager 上配置）
 */
USTRUCT(BlueprintType)
struct FGridMapConfig
{
	GENERATED_BODY()

	// 地图宽度（格子数）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1"))
	int32 MapWidth = 500;

	// 地图高度（格子数）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1"))
	int32 MapHeight = 500;

	// 每格世界尺寸（UU）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "10.0"))
	float TileSize = 100.0f;

	// 一般树木的基础耐久
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1"))
	int32 TreeHealth = 100;

	// 高大树木的基础耐久
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "1"))
	int32 LargeTreeHealth = 300;
};
