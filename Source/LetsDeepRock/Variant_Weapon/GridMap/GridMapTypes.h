// 网格地图系统 - 类型定义

#pragma once

#include "CoreMinimal.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "GridMapTypes.generated.h"

class AGridMapManager;
class UGridMapGenerator;

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

	/** Empty 不遮挡视线，其余当前地形类型均遮挡。 */
	bool BlocksVision() const
	{
		return TileType != EGridTileType::Empty;
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

/** 客户端重建确定性基础地图所需的版本化描述。 */
USTRUCT(BlueprintType)
struct FGridMapBaselineState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	int32 SchemaVersion = 1;

	UPROPERTY(BlueprintReadOnly)
	int32 GenerationVersion = 0;

	UPROPERTY(BlueprintReadOnly)
	FGridMapConfig Config;

	UPROPERTY(BlueprintReadOnly)
	TSubclassOf<UGridMapGenerator> GeneratorClass;

	/** 服务器实际使用的随机种子；客户端不得自行生成。 */
	UPROPERTY(BlueprintReadOnly)
	int32 EffectiveSeed = 0;

	UPROPERTY(BlueprintReadOnly)
	bool bGenerated = false;
};

/** 单格相对确定性基础地图的最新权威状态。 */
USTRUCT(BlueprintType)
struct FGridTileDeltaItem : public FFastArraySerializerItem
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	int32 GridX = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 GridY = 0;

	UPROPERTY(BlueprintReadOnly)
	EGridTileType TileType = EGridTileType::Empty;

	UPROPERTY(BlueprintReadOnly)
	int32 Health = 0;

	UPROPERTY(BlueprintReadOnly)
	int32 MaxHealth = 0;

	/** 防止旧地图世代的 Delta 被应用到新基础地图。 */
	UPROPERTY(BlueprintReadOnly)
	int32 GenerationVersion = 0;

	/** 同一地图世代内由服务器分配的单调递增版本。 */
	UPROPERTY()
	uint32 Revision = 0;
};

/** 只复制发生变化的格子，并为 Late Join 保留每格最新状态。 */
USTRUCT(BlueprintType)
struct FGridTileDeltaArray : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	TArray<FGridTileDeltaItem> Items;

	bool NetDeltaSerialize(FNetDeltaSerializeInfo& DeltaParams)
	{
		return FastArrayDeltaSerialize<FGridTileDeltaItem, FGridTileDeltaArray>(Items, DeltaParams, *this);
	}

	void PostReplicatedAdd(const TArrayView<int32>& AddedIndices, int32 FinalSize);
	void PostReplicatedChange(const TArrayView<int32>& ChangedIndices, int32 FinalSize);

	AGridMapManager* Owner = nullptr;
};

template<>
struct TStructOpsTypeTraits<FGridTileDeltaArray> : public TStructOpsTypeTraitsBase2<FGridTileDeltaArray>
{
	enum
	{
		WithNetDeltaSerializer = true,
	};
};
