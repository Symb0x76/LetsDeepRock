// 网格地图系统 - 渲染层实现

#include "GridMapRenderer.h"
#include "GridMapData.h"
#include "Components/HierarchicalInstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"

UGridMapRenderer::UGridMapRenderer()
{
	PrimaryComponentTick.bCanEverTick = false;

	// 默认使用引擎内置 Cube Mesh
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CubeMesh(
		TEXT("/Engine/BasicShapes/Cube.Cube"));
	if (CubeMesh.Succeeded())
	{
		TileMesh = CubeMesh.Object;
	}
}

void UGridMapRenderer::BeginPlay()
{
	Super::BeginPlay();
}

void UGridMapRenderer::BuildFromMapData(UGridMapData* MapData)
{
	if (!MapData || !TileMesh)
	{
		UE_LOG(LogTemp, Error, TEXT("[GridMap Renderer] MapData 或 TileMesh 为空，无法渲染。"));
		return;
	}

	const FGridMapConfig& Config = MapData->GetConfig();
	CachedMapWidth = Config.MapWidth;

	// 清理旧的 HISM（如果重新生成）
	if (TreeHISM) { TreeHISM->ClearInstances(); TreeHISM->DestroyComponent(); TreeHISM = nullptr; }
	if (RockHISM) { RockHISM->ClearInstances(); RockHISM->DestroyComponent(); RockHISM = nullptr; }
	if (LargeTreeHISM) { LargeTreeHISM->ClearInstances(); LargeTreeHISM->DestroyComponent(); LargeTreeHISM = nullptr; }

	TreeInstanceMap.Empty();
	RockInstanceMap.Empty();
	LargeTreeInstanceMap.Empty();

	// 创建 HISM 组件
	TreeHISM = CreateHISMForType(EGridTileType::Tree);
	RockHISM = CreateHISMForType(EGridTileType::Rock);
	LargeTreeHISM = CreateHISMForType(EGridTileType::LargeTree);

	// 遍历所有格子，为非空格子添加实例
	const TArray<FGridTileData>& AllTiles = MapData->GetAllTiles();

	for (const FGridTileData& Tile : AllTiles)
	{
		AddTileInstance(MapData, Tile);
	}

	UE_LOG(LogTemp, Log, TEXT("[GridMap Renderer] 渲染完成 - 树木实例=%d，岩石实例=%d，高大树木实例=%d"),
		TreeHISM ? TreeHISM->GetInstanceCount() : 0,
		RockHISM ? RockHISM->GetInstanceCount() : 0,
		LargeTreeHISM ? LargeTreeHISM->GetInstanceCount() : 0);
}

void UGridMapRenderer::RefreshTileInstance(UGridMapData* MapData, int32 GridX, int32 GridY)
{
	if (!MapData)
	{
		return;
	}

	RemoveTileInstance(GridX, GridY);
	if (const FGridTileData* Tile = MapData->GetTile(GridX, GridY))
	{
		AddTileInstance(MapData, *Tile);
	}
}

void UGridMapRenderer::AddTileInstance(UGridMapData* MapData, const FGridTileData& Tile)
{
	if (!MapData || Tile.TileType == EGridTileType::Empty)
	{
		return;
	}

	UHierarchicalInstancedStaticMeshComponent* TargetHISM = GetHISMForType(Tile.TileType);
	TMap<int32, int32>* TargetMap = GetInstanceMapForType(Tile.TileType);
	if (!TargetHISM || !TargetMap)
	{
		return;
	}

	const FGridMapConfig& Config = MapData->GetConfig();
	const FVector WorldPos = MapData->GridToWorld(Tile.GridX, Tile.GridY);
	const float Height = GetHeightForType(Tile.TileType);
	const float ScaleXY = Config.TileSize / 100.0f;
	const float ScaleZ = Height / 100.0f;

	FTransform InstanceTransform;
	InstanceTransform.SetLocation(FVector(WorldPos.X, WorldPos.Y, Height * 0.5f));
	InstanceTransform.SetScale3D(FVector(ScaleXY, ScaleXY, ScaleZ));

	const int32 InstanceIndex = TargetHISM->AddInstance(InstanceTransform, true);
	const int32 Key = Tile.GridY * CachedMapWidth + Tile.GridX;
	TargetMap->Add(Key, InstanceIndex);
}

void UGridMapRenderer::RemoveTileInstance(int32 GridX, int32 GridY)
{
	// 尝试从所有类型的映射中查找并移除
	const int32 Key = GridY * CachedMapWidth + GridX;

	// 辅助 lambda：在指定 HISM 和 Map 中移除
	auto TryRemove = [&](UHierarchicalInstancedStaticMeshComponent* HISM, TMap<int32, int32>& Map) -> bool
	{
		const int32* FoundIndex = Map.Find(Key);
		if (!FoundIndex || !HISM)
		{
			return false;
		}

		const int32 IndexToRemove = *FoundIndex;
		HISM->RemoveInstance(IndexToRemove);

		// RemoveInstance 后，最后一个实例被 swap 到了被删除的位置
		// 需要更新映射中原来指向最后一个位置的条目
		const int32 LastIndex = HISM->GetInstanceCount(); // 删除后的 Count 就是原来的最后一个索引
		Map.Remove(Key);

		// 查找并更新被 swap 过来的实例的映射
		for (auto& Pair : Map)
		{
			if (Pair.Value == LastIndex)
			{
				Pair.Value = IndexToRemove;
				break;
			}
		}

		return true;
	};

	if (TryRemove(TreeHISM, TreeInstanceMap)) return;
	if (TryRemove(RockHISM, RockInstanceMap)) return;
	if (TryRemove(LargeTreeHISM, LargeTreeInstanceMap)) return;
}

UHierarchicalInstancedStaticMeshComponent* UGridMapRenderer::CreateHISMForType(EGridTileType Type)
{
	AActor* Owner = GetOwner();
	if (!Owner)
	{
		return nullptr;
	}

	UHierarchicalInstancedStaticMeshComponent* HISM =
		NewObject<UHierarchicalInstancedStaticMeshComponent>(Owner);
	HISM->SetStaticMesh(TileMesh);
	HISM->SetMobility(EComponentMobility::Movable); // Movable 以支持运行时移除实例
	HISM->SetCullDistances(0, 50000); // 500m 裁剪距离

	// 启用碰撞（阻挡玩家和 AI 移动，影响 NavMesh）
	HISM->SetCollisionEnabled(ECollisionEnabled::QueryAndPhysics);
	HISM->SetCollisionObjectType(ECollisionChannel::ECC_WorldStatic);
	HISM->SetCollisionResponseToAllChannels(ECollisionResponse::ECR_Block);
	HISM->SetGenerateOverlapEvents(false);
	HISM->SetCanEverAffectNavigation(true); // 影响 NavMesh

	// 设置材质：优先用户配置，否则创建默认颜色材质
	UMaterialInterface* Mat = GetMaterialForType(Type);
	if (Mat)
	{
		HISM->SetMaterial(0, Mat);
	}
	else
	{
		// 无配置材质时，创建动态材质实例并赋予默认颜色
		UMaterialInterface* BaseMat = HISM->GetMaterial(0);
		if (BaseMat)
		{
			UMaterialInstanceDynamic* DynMat = UMaterialInstanceDynamic::Create(BaseMat, Owner);
			if (DynMat)
			{
				FLinearColor Color;
				switch (Type)
				{
				case EGridTileType::Tree:      Color = FLinearColor(0.1f, 0.7f, 0.1f); break;  // 绿色
				case EGridTileType::Rock:      Color = FLinearColor(0.5f, 0.5f, 0.5f); break;  // 灰色
				case EGridTileType::LargeTree: Color = FLinearColor(0.05f, 0.4f, 0.05f); break; // 深绿
				default:                       Color = FLinearColor::White; break;
				}
				DynMat->SetVectorParameterValue(TEXT("BaseColor"), Color);
				DynMat->SetVectorParameterValue(TEXT("Color"), Color); // 兼容不同材质参数名
				HISM->SetMaterial(0, DynMat);
			}
		}
	}

	HISM->RegisterComponent();
	HISM->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);

	return HISM;
}

float UGridMapRenderer::GetHeightForType(EGridTileType Type) const
{
	switch (Type)
	{
	case EGridTileType::Tree:      return TreeHeight;
	case EGridTileType::Rock:      return RockHeight;
	case EGridTileType::LargeTree: return LargeTreeHeight;
	default:                       return 0.0f;
	}
}

UMaterialInterface* UGridMapRenderer::GetMaterialForType(EGridTileType Type) const
{
	switch (Type)
	{
	case EGridTileType::Tree:      return TreeMaterial;
	case EGridTileType::Rock:      return RockMaterial;
	case EGridTileType::LargeTree: return LargeTreeMaterial;
	default:                       return nullptr;
	}
}

TMap<int32, int32>* UGridMapRenderer::GetInstanceMapForType(EGridTileType Type)
{
	switch (Type)
	{
	case EGridTileType::Tree:      return &TreeInstanceMap;
	case EGridTileType::Rock:      return &RockInstanceMap;
	case EGridTileType::LargeTree: return &LargeTreeInstanceMap;
	default:                       return nullptr;
	}
}

UHierarchicalInstancedStaticMeshComponent* UGridMapRenderer::GetHISMForType(EGridTileType Type)
{
	switch (Type)
	{
	case EGridTileType::Tree:      return TreeHISM;
	case EGridTileType::Rock:      return RockHISM;
	case EGridTileType::LargeTree: return LargeTreeHISM;
	default:                       return nullptr;
	}
}
