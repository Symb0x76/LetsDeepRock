// 网格地图系统 - 类型定义

#pragma once

#include "CoreMinimal.h"
#include "Net/Serialization/FastArraySerializer.h"
#include "GridMapTypes.generated.h"

class AGridMapManager;
class UGridMapGenerator;

/** Renderer 与导航脏区共同使用的固定区块尺寸。 */
inline constexpr int32 GridMapChunkSize = 64;

/** 仅服务端使用的同帧导航脏区块合并器。 */
struct FGridNavigationDirtyTracker
{
	void MarkTile(int32 GridX, int32 GridY, bool bAuthoritative)
	{
		if (bAuthoritative)
		{
			DirtyChunks.Add(FIntPoint(GridX / GridMapChunkSize, GridY / GridMapChunkSize));
		}
	}

	template <typename FlushFunction>
	int32 Flush(bool bAuthoritative, FlushFunction&& FlushChunk)
	{
		if (!bAuthoritative)
		{
			DirtyChunks.Reset();
			return 0;
		}

		const TArray<FIntPoint> PendingChunks = DirtyChunks.Array();
		DirtyChunks.Reset();
		for (const FIntPoint& ChunkCoord : PendingChunks)
		{
			FlushChunk(ChunkCoord);
		}
		return PendingChunks.Num();
	}

	int32 Num() const { return DirtyChunks.Num(); }

private:
	TSet<FIntPoint> DirtyChunks;
};

/**
 * 地面格子类型枚举
 */
UENUM(BlueprintType)
enum class EGridTileType : uint8
{
	Empty = 0		UMETA(DisplayName = "空地"),
	Tree = 1		UMETA(DisplayName = "一般树木"),
	Rock = 2		UMETA(DisplayName = "岩石"),
	LargeTree = 3	UMETA(DisplayName = "高大树木"),
	Uncarved = 4	UMETA(DisplayName = "未开凿区域")
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
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, meta = (ClampMin = "1"))
	int32 MapWidth = 500;

	// 地图高度（格子数）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, meta = (ClampMin = "1"))
	int32 MapHeight = 500;

	// 每格世界尺寸（UU）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, meta = (ClampMin = "10.0"))
	float TileSize = 100.0f;

	// 一般树木的基础耐久
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, meta = (ClampMin = "1"))
	int32 TreeHealth = 100;

	// 高大树木的基础耐久
	UPROPERTY(EditAnywhere, BlueprintReadWrite, SaveGame, meta = (ClampMin = "1"))
	int32 LargeTreeHealth = 300;
};

inline constexpr int32 GridMapRecoveryStateVersion = 2;

UENUM()
enum class EGridMapRoomPurpose : uint8
{
	Spawn,
	Connector,
	Combat,
	Objective,
	Branch
};

USTRUCT()
struct FGridMapRecipeRoom
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	FSoftObjectPath TemplatePath;

	UPROPERTY(SaveGame)
	int32 TemplateVersion = 0;

	UPROPERTY(SaveGame)
	uint64 TemplateContentHash = 0;

	UPROPERTY(SaveGame)
	FIntPoint Origin = FIntPoint::ZeroValue;

	UPROPERTY(SaveGame)
	FIntPoint Size = FIntPoint::ZeroValue;

	UPROPERTY(SaveGame)
	EGridMapRoomPurpose Purpose = EGridMapRoomPurpose::Connector;
};

USTRUCT()
struct FGridMapRecipeConnection
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	int32 FromRoom = INDEX_NONE;

	UPROPERTY(SaveGame)
	int32 ToRoom = INDEX_NONE;

	UPROPERTY(SaveGame)
	int32 CorridorWidth = 1;

	/** 已含起终出口的确定性折线。 */
	UPROPERTY(SaveGame)
	TArray<FIntPoint> Points;
};

USTRUCT()
struct FGridMapRecipeMarker
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	uint8 Type = 0;

	UPROPERTY(SaveGame)
	FIntPoint GridCoord = FIntPoint::ZeroValue;
};

USTRUCT()
struct FGridMapGenerationRecipe
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	TArray<FGridMapRecipeRoom> Rooms;

	UPROPERTY(SaveGame)
	TArray<FGridMapRecipeConnection> Connections;

	UPROPERTY(SaveGame)
	TArray<FGridMapRecipeMarker> Markers;

	bool IsEmpty() const { return Rooms.IsEmpty(); }
};

/** SaveGame 中单格相对 deterministic baseline 的最新状态。 */
USTRUCT()
struct FGridTileRecoveryState
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	int32 GridX = 0;

	UPROPERTY(SaveGame)
	int32 GridY = 0;

	UPROPERTY(SaveGame)
	EGridTileType TileType = EGridTileType::Empty;

	UPROPERTY(SaveGame)
	int32 Health = 0;

	UPROPERTY(SaveGame)
	int32 MaxHealth = 0;

	UPROPERTY(SaveGame)
	int32 GenerationVersion = 0;

	UPROPERTY(SaveGame)
	uint32 Revision = 0;
};

/** GridMap owner 导出的完整恢复契约：确定性基线 + 稀疏 Tile Delta。 */
USTRUCT()
struct FGridMapRecoveryState
{
	GENERATED_BODY()

	UPROPERTY(SaveGame)
	int32 SchemaVersion = GridMapRecoveryStateVersion;

	UPROPERTY(SaveGame)
	int32 BaselineSchemaVersion = 2;

	UPROPERTY(SaveGame)
	int32 GenerationVersion = 0;

	UPROPERTY(SaveGame)
	FGridMapConfig Config;

	UPROPERTY(SaveGame)
	FSoftClassPath GeneratorClass;

	UPROPERTY(SaveGame)
	int32 EffectiveSeed = 0;

	UPROPERTY(SaveGame)
	int32 AlgorithmVersion = 0;

	UPROPERTY(SaveGame)
	int32 TemplateSetVersion = 0;

	UPROPERTY(SaveGame)
	FGridMapGenerationRecipe Recipe;

	UPROPERTY(SaveGame)
	uint64 ResultHash = 0;

	UPROPERTY(SaveGame)
	bool bGenerated = false;

	UPROPERTY(SaveGame)
	TArray<FGridTileRecoveryState> TileDeltas;

	UPROPERTY(SaveGame)
	uint32 NextTileRevision = 0;
};

/** 客户端重建确定性基础地图所需的内部 C++ 复制协议。 */
USTRUCT()
struct FGridMapBaselineState
{
	GENERATED_BODY()

	UPROPERTY()
	int32 SchemaVersion = 2;

	UPROPERTY()
	int32 GenerationVersion = 0;

	UPROPERTY()
	FGridMapConfig Config;

	UPROPERTY()
	TSubclassOf<UGridMapGenerator> GeneratorClass;

	/** 服务器实际使用的随机种子；客户端不得自行生成。 */
	UPROPERTY()
	int32 EffectiveSeed = 0;

	UPROPERTY()
	int32 AlgorithmVersion = 0;

	UPROPERTY()
	int32 TemplateSetVersion = 0;

	UPROPERTY()
	FGridMapGenerationRecipe Recipe;

	UPROPERTY()
	uint64 ResultHash = 0;

	UPROPERTY()
	bool bGenerated = false;
};

/** 单格相对确定性基础地图的内部 C++ Fast Array 协议状态。 */
USTRUCT()
struct FGridTileDeltaItem : public FFastArraySerializerItem
{
	GENERATED_BODY()

	UPROPERTY()
	int32 GridX = 0;

	UPROPERTY()
	int32 GridY = 0;

	UPROPERTY()
	EGridTileType TileType = EGridTileType::Empty;

	UPROPERTY()
	int32 Health = 0;

	UPROPERTY()
	int32 MaxHealth = 0;

	/** 防止旧地图世代的 Delta 被应用到新基础地图。 */
	UPROPERTY()
	int32 GenerationVersion = 0;

	/** 同一地图世代内由服务器分配的单调递增版本。 */
	UPROPERTY()
	uint32 Revision = 0;
};

/** 内部 C++ Fast Array：只复制发生变化的格子，并为 Late Join 保留每格最新状态。 */
USTRUCT()
struct FGridTileDeltaArray : public FFastArraySerializer
{
	GENERATED_BODY()

	UPROPERTY()
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
