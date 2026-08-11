// 网格地图系统 - 64x64 区块渲染层

#pragma once
#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GridMapTypes.h"
#include "GridMapRenderer.generated.h"

class UHierarchicalInstancedStaticMeshComponent;
class UGridMapData;

UCLASS(BlueprintType, ClassGroup = (GridMap), meta = (BlueprintSpawnableComponent))
class LETSDEEPROCK_API UGridMapRenderer : public UActorComponent
{
	GENERATED_BODY()

public:
	UGridMapRenderer();

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Renderer")
	TObjectPtr<UStaticMesh> TileMesh;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Renderer")
	TObjectPtr<UMaterialInterface> TreeMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Renderer")
	TObjectPtr<UMaterialInterface> RockMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Renderer")
	TObjectPtr<UMaterialInterface> LargeTreeMaterial;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Renderer")
	float TreeHeight = 80.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Renderer")
	float RockHeight = 100.0f;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Renderer")
	float LargeTreeHeight = 150.0f;

	static constexpr int32 ChunkSize = GridMapChunkSize;

	void BuildFromMapData(UGridMapData* MapData);
	void MarkDirtyChunk(int32 GridX, int32 GridY);
	void FlushDirtyChunks();

	/** 兼容现有调用；现在只标记所属区块，不逐格维护实例。 */
	void RemoveTileInstance(int32 GridX, int32 GridY);
	void RefreshTileInstance(UGridMapData* MapData, int32 GridX, int32 GridY);

	int32 GetActiveChunkCount() const { return Chunks.Num(); }
	int32 GetHISMComponentCount() const { return DynamicHISMs.Num(); }
	int32 GetTotalInstanceCount() const;
	int32 GetInstanceCountForType(EGridTileType Type) const;
	int32 GetDirtyChunkCount() const { return DirtyChunks.Num(); }
	int32 GetLastFlushChunkCount() const { return LastFlushChunkCount; }

private:
	struct FChunkRenderData
	{
		TArray<FTransform> TreeTransforms;
		TArray<FTransform> RockTransforms;
		TArray<FTransform> LargeTreeTransforms;
		TObjectPtr<UHierarchicalInstancedStaticMeshComponent> TreeHISM;
		TObjectPtr<UHierarchicalInstancedStaticMeshComponent> RockHISM;
		TObjectPtr<UHierarchicalInstancedStaticMeshComponent> LargeTreeHISM;
	};

	UPROPERTY(Transient)
	TObjectPtr<UGridMapData> CachedMapData;

	/** 强引用所有动态组件；区块结构仅负责定位。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UHierarchicalInstancedStaticMeshComponent>> DynamicHISMs;

	TMap<FIntPoint, FChunkRenderData> Chunks;
	TSet<FIntPoint> DirtyChunks;
	int32 LastFlushChunkCount = 0;

	void ClearAllChunks();
	void CollectChunk(const FIntPoint& ChunkCoord, FChunkRenderData& OutData) const;
	void RebuildChunk(const FIntPoint& ChunkCoord);
	void DestroyChunkComponents(FChunkRenderData& Chunk);
	UHierarchicalInstancedStaticMeshComponent* CreateChunkHISM(const FIntPoint& ChunkCoord, EGridTileType Type, const TArray<FTransform>& Transforms);
	FTransform MakeTileTransform(const FGridTileData& Tile) const;
	float GetHeightForType(EGridTileType Type) const;
	UMaterialInterface* GetMaterialForType(EGridTileType Type) const;
};
