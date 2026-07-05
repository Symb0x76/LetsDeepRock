// 网格地图系统 - 管理器实现

#include "GridMapManager.h"
#include "GridMapData.h"
#include "GridMapGenerator.h"
#include "GridMapRenderer.h"
#include "NavigationSystem.h"

AGridMapManager::AGridMapManager()
{
	PrimaryActorTick.bCanEverTick = false;

	// 创建根组件
	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent"));
	SetRootComponent(Root);

	// 创建渲染器组件
	MapRenderer = CreateDefaultSubobject<UGridMapRenderer>(TEXT("MapRenderer"));

	// 默认使用 Simple 生成器
	GeneratorClass = UGridMapGenerator_Simple::StaticClass();
}

void AGridMapManager::BeginPlay()
{
	Super::BeginPlay();

	if (bAutoGenerateOnBeginPlay)
	{
		GenerateMap();
	}
}

void AGridMapManager::GenerateMap()
{
	// 1. 创建地图数据
	MapData = NewObject<UGridMapData>(this);
	MapData->Initialize(MapConfig);

	UE_LOG(LogTemp, Log, TEXT("[GridMap] 地图数据初始化完成：%dx%d，格子尺寸=%.0f UU"),
		MapConfig.MapWidth, MapConfig.MapHeight, MapConfig.TileSize);

	// 2. 创建并执行生成器
	if (GeneratorClass)
	{
		Generator = NewObject<UGridMapGenerator>(this, GeneratorClass);
		Generator->Generate(MapData);
	}
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[GridMap] 未设置 GeneratorClass，地图为全空地。"));
	}

	// 3. 触发渲染
	if (MapRenderer)
	{
		MapRenderer->BuildFromMapData(MapData);
	}

	// 4. NavMesh 会自动根据碰撞体更新（HISM 带碰撞）
	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	if (NavSys)
	{
		NavSys->Build();
	}

	UE_LOG(LogTemp, Log, TEXT("[GridMap] 地图生成流程完成。"));
}

bool AGridMapManager::GetTileAtWorldLocation(const FVector& WorldLocation, FGridTileData& OutTileData) const
{
	if (!MapData)
	{
		return false;
	}

	int32 X, Y;
	if (!MapData->WorldToGrid(WorldLocation, X, Y))
	{
		return false;
	}

	const FGridTileData* Tile = MapData->GetTile(X, Y);
	if (!Tile)
	{
		return false;
	}

	OutTileData = *Tile;
	return true;
}

bool AGridMapManager::DamageTile(int32 GridX, int32 GridY, int32 DamageAmount)
{
	if (!MapData)
	{
		return false;
	}

	FGridTileData* Tile = MapData->GetTileMutable(GridX, GridY);
	if (!Tile)
	{
		return false;
	}

	// 不可破坏的格子直接返回
	if (!Tile->IsDestructible())
	{
		return false;
	}

	// 扣血
	Tile->Health = FMath::Max(0, Tile->Health - DamageAmount);

	// 判断是否被破坏
	if (Tile->Health <= 0)
	{
		// 破坏：变为空地
		Tile->TileType = EGridTileType::Empty;
		Tile->Health = 0;
		Tile->MaxHealth = 0;

		OnTileDestroyedInternal(GridX, GridY);
		OnTileDestroyed.Broadcast(GridX, GridY);

		UE_LOG(LogTemp, Log, TEXT("[GridMap] 格子 (%d, %d) 被破坏，变为空地。"), GridX, GridY);
		return true;
	}

	return false;
}

bool AGridMapManager::DamageTileAtWorldLocation(const FVector& WorldLocation, int32 DamageAmount)
{
	if (!MapData)
	{
		return false;
	}

	int32 X, Y;
	if (!MapData->WorldToGrid(WorldLocation, X, Y))
	{
		return false;
	}

	return DamageTile(X, Y, DamageAmount);
}

void AGridMapManager::OnTileDestroyedInternal(int32 GridX, int32 GridY)
{
	// 移除渲染实例（同时移除碰撞，因为 HISM 碰撞和实例绑定）
	if (MapRenderer)
	{
		MapRenderer->RemoveTileInstance(GridX, GridY);
	}

	// 局部更新 NavMesh
	if (MapData)
	{
		const FVector WorldPos = MapData->GridToWorld(GridX, GridY);
		UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
		if (NavSys)
		{
			// 标记局部区域脏，触发异步 NavMesh 重建
			const FBox DirtyBounds(
				WorldPos - FVector(MapData->GetConfig().TileSize),
				WorldPos + FVector(MapData->GetConfig().TileSize)
			);
			NavSys->AddDirtyArea(DirtyBounds, ENavigationDirtyFlag::All);
		}
	}
}
