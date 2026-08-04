// 网格地图系统 - 渲染层

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GridMapTypes.h"
#include "GridMapRenderer.generated.h"

class UHierarchicalInstancedStaticMeshComponent;
class UGridMapData;

/**
 * UGridMapRenderer
 * 
 * 网格地图渲染组件。挂载在 GridMapManager 上，负责：
 * 1. 使用 HISM（Hierarchical Instanced Static Mesh）批量渲染障碍格子
 * 2. 每种格子类型使用独立的 HISM 组件（不同材质/颜色）
 * 3. 支持运行时移除单个实例（格子被破坏时）
 * 
 * 空地不渲染，仅渲染非空类型的格子。
 * 地面底板由关卡中单独放置的平面 Mesh 负责。
 */
UCLASS(BlueprintType, ClassGroup = (GridMap), meta = (BlueprintSpawnableComponent))
class LETSDEEPROCK_API UGridMapRenderer : public UActorComponent
{
	GENERATED_BODY()

public:
	UGridMapRenderer();

	// ===== 配置 =====

	/** 占位方块 Mesh（默认使用引擎内置 Cube） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Renderer")
	TObjectPtr<UStaticMesh> TileMesh;

	/** 一般树木材质 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Renderer")
	TObjectPtr<UMaterialInterface> TreeMaterial;

	/** 岩石材质 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Renderer")
	TObjectPtr<UMaterialInterface> RockMaterial;

	/** 高大树木材质 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Renderer")
	TObjectPtr<UMaterialInterface> LargeTreeMaterial;

	/** 一般树木方块高度（UU） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Renderer")
	float TreeHeight = 80.0f;

	/** 岩石方块高度（UU） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Renderer")
	float RockHeight = 100.0f;

	/** 高大树木方块高度（UU） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Renderer")
	float LargeTreeHeight = 150.0f;

	// ===== 接口 =====

	/** 根据地图数据构建全部渲染实例 */
	void BuildFromMapData(UGridMapData* MapData);

	/** 移除指定格子坐标对应的渲染实例 */
	void RemoveTileInstance(int32 GridX, int32 GridY);

	/** 根据数据层中的最新状态局部重建一个格子的实例。 */
	void RefreshTileInstance(UGridMapData* MapData, int32 GridX, int32 GridY);

protected:
	virtual void BeginPlay() override;

private:
	/** 为指定格子类型创建 HISM 组件 */
	UHierarchicalInstancedStaticMeshComponent* CreateHISMForType(EGridTileType Type);

	/** 获取格子类型对应的高度 */
	float GetHeightForType(EGridTileType Type) const;

	/** 获取格子类型对应的材质 */
	UMaterialInterface* GetMaterialForType(EGridTileType Type) const;

	// HISM 组件（每种类型一个）
	UPROPERTY()
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> TreeHISM;

	UPROPERTY()
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> RockHISM;

	UPROPERTY()
	TObjectPtr<UHierarchicalInstancedStaticMeshComponent> LargeTreeHISM;

	// 格子坐标 → 实例索引的映射（用于破坏时快速定位）
	// Key: GridY * MapWidth + GridX, Value: 实例 Index
	TMap<int32, int32> TreeInstanceMap;
	TMap<int32, int32> RockInstanceMap;
	TMap<int32, int32> LargeTreeInstanceMap;

	// 缓存的地图宽度（用于坐标转 Key）
	int32 CachedMapWidth = 0;

	/** 获取对应类型的实例映射 */
	TMap<int32, int32>* GetInstanceMapForType(EGridTileType Type);

	/** 获取对应类型的 HISM */
	UHierarchicalInstancedStaticMeshComponent* GetHISMForType(EGridTileType Type);

	/** 为单个非空格子添加 HISM 实例并更新索引映射。 */
	void AddTileInstance(UGridMapData* MapData, const FGridTileData& Tile);
};
