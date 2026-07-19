// 网格地图系统 - 服务器权威管理器实现

#include "GridMapManager.h"
#include "GridMapData.h"
#include "GridMapGenerator.h"
#include "GridMapRenderer.h"
#include "NavigationSystem.h"
#include "Net/UnrealNetwork.h"

void FGridTileDeltaArray::PostReplicatedAdd(const TArrayView<int32>& AddedIndices, int32 FinalSize)
{
	if (!Owner)
	{
		return;
	}

	for (const int32 Index : AddedIndices)
	{
		if (Items.IsValidIndex(Index))
		{
			Owner->HandleReplicatedTileDelta(Items[Index]);
		}
	}
}

void FGridTileDeltaArray::PostReplicatedChange(const TArrayView<int32>& ChangedIndices, int32 FinalSize)
{
	if (!Owner)
	{
		return;
	}

	for (const int32 Index : ChangedIndices)
	{
		if (Items.IsValidIndex(Index))
		{
			Owner->HandleReplicatedTileDelta(Items[Index]);
		}
	}
}

AGridMapManager::AGridMapManager()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = true;
	bAlwaysRelevant = true;

	USceneComponent* Root = CreateDefaultSubobject<USceneComponent>(TEXT("RootComponent"));
	SetRootComponent(Root);

	MapRenderer = CreateDefaultSubobject<UGridMapRenderer>(TEXT("MapRenderer"));
	GeneratorClass = UGridMapGenerator_Simple::StaticClass();
	TileDeltas.Owner = this;
}

void AGridMapManager::BeginPlay()
{
	Super::BeginPlay();
	TileDeltas.Owner = this;

	if (HasAuthority() && bAutoGenerateOnBeginPlay)
	{
		GenerateMap();
	}
	else if (!HasAuthority() && BaselineState.bGenerated)
	{
		BuildBaselineMap(BaselineState);
		ApplyAllReplicatedDeltas();
	}
}

void AGridMapManager::GenerateMap()
{
	if (!HasAuthority())
	{
		return;
	}

	FGridMapBaselineState NewState;
	NewState.SchemaVersion = 1;
	NewState.GenerationVersion = BaselineState.GenerationVersion + 1;
	NewState.Config = MapConfig;
	NewState.GeneratorClass = GeneratorClass;
	NewState.bGenerated = true;

	Generator = GeneratorClass ? NewObject<UGridMapGenerator>(this, GeneratorClass) : nullptr;
	if (UGridMapGenerator_Simple* SimpleGenerator = Cast<UGridMapGenerator_Simple>(Generator))
	{
		NewState.EffectiveSeed = SimpleGenerator->Seed != 0
			? SimpleGenerator->Seed
			: FMath::RandRange(1, MAX_int32);
	}

	BaselineState = NewState;
	TileDeltas.Items.Reset();
	TileDeltas.MarkArrayDirty();
	NextTileRevision = 0;

	BuildBaselineMap(BaselineState);
	ForceNetUpdate();
}

void AGridMapManager::BuildBaselineMap(const FGridMapBaselineState& State)
{
	if (!State.bGenerated || State.SchemaVersion != 1)
	{
		return;
	}

	MapData = NewObject<UGridMapData>(this);
	MapData->Initialize(State.Config);
	Generator = State.GeneratorClass ? NewObject<UGridMapGenerator>(this, State.GeneratorClass) : nullptr;

	if (UGridMapGenerator_Simple* SimpleGenerator = Cast<UGridMapGenerator_Simple>(Generator))
	{
		SimpleGenerator->Seed = State.EffectiveSeed;
	}

	if (Generator)
	{
		Generator->Generate(MapData);
	}

	if (MapRenderer)
	{
		MapRenderer->BuildFromMapData(MapData);
	}

	if (HasAuthority())
	{
		if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld()))
		{
			NavSys->Build();
		}
	}

	UE_LOG(LogTemp, Log, TEXT("[GridMap] 基础地图世代 %d 构建完成，Seed=%d。"),
		State.GenerationVersion, State.EffectiveSeed);
}

void AGridMapManager::OnRep_BaselineState()
{
	BuildBaselineMap(BaselineState);
	ApplyAllReplicatedDeltas();
}

bool AGridMapManager::GetTileAtWorldLocation(const FVector& WorldLocation, FGridTileData& OutTileData) const
{
	if (!MapData)
	{
		return false;
	}

	int32 GridX = 0;
	int32 GridY = 0;
	if (!MapData->WorldToGrid(WorldLocation, GridX, GridY))
	{
		return false;
	}

	const FGridTileData* Tile = MapData->GetTile(GridX, GridY);
	if (!Tile)
	{
		return false;
	}

	OutTileData = *Tile;
	return true;
}

bool AGridMapManager::DamageTile(int32 GridX, int32 GridY, int32 DamageAmount)
{
	if (!HasAuthority() || !MapData || DamageAmount <= 0)
	{
		return false;
	}

	const FGridTileData* Tile = MapData->GetTile(GridX, GridY);
	if (!Tile || !Tile->IsDestructible())
	{
		return false;
	}

	const int32 NewHealth = FMath::Max(0, Tile->Health - DamageAmount);
	const bool bDestroyed = NewHealth == 0;
	CommitTileState(GridX, GridY,
		bDestroyed ? EGridTileType::Empty : Tile->TileType,
		NewHealth,
		bDestroyed ? 0 : Tile->MaxHealth);

	return bDestroyed;
}

bool AGridMapManager::DamageTileAtWorldLocation(const FVector& WorldLocation, int32 DamageAmount)
{
	if (!HasAuthority() || !MapData)
	{
		return false;
	}

	int32 GridX = 0;
	int32 GridY = 0;
	return MapData->WorldToGrid(WorldLocation, GridX, GridY)
		&& DamageTile(GridX, GridY, DamageAmount);
}

bool AGridMapManager::DamageTilesInRadius(const FVector& Center, float Radius, float MaxDamage)
{
	if (!HasAuthority() || !MapData || Radius <= 0.0f || MaxDamage <= 0.0f)
	{
		return false;
	}

	const FGridMapConfig& Config = MapData->GetConfig();
	int32 CenterX = 0;
	int32 CenterY = 0;
	MapData->WorldToGrid(Center, CenterX, CenterY);
	const int32 GridRadius = FMath::CeilToInt(Radius / Config.TileSize) + 1;
	const float RadiusSquared = FMath::Square(Radius);
	bool bAnyDamage = false;

	for (int32 GridY = FMath::Max(0, CenterY - GridRadius);
		GridY <= FMath::Min(Config.MapHeight - 1, CenterY + GridRadius); ++GridY)
	{
		for (int32 GridX = FMath::Max(0, CenterX - GridRadius);
			GridX <= FMath::Min(Config.MapWidth - 1, CenterX + GridRadius); ++GridX)
		{
			const float DistanceSquared = FVector::DistSquared2D(MapData->GridToWorld(GridX, GridY), Center);
			if (DistanceSquared > RadiusSquared)
			{
				continue;
			}

			const FGridTileData* Tile = MapData->GetTile(GridX, GridY);
			if (!Tile || !Tile->IsDestructible())
			{
				continue;
			}

			const float Attenuation = 1.0f - FMath::Sqrt(DistanceSquared) / Radius;
			const int32 DamageAmount = FMath::RoundToInt(MaxDamage * FMath::Clamp(Attenuation, 0.0f, 1.0f));
			if (DamageAmount > 0)
			{
				DamageTile(GridX, GridY, DamageAmount);
				bAnyDamage = true;
			}
		}
	}

	return bAnyDamage;
}

bool AGridMapManager::DamageTilesInRect(const FVector& Center, float HalfWidth,
	float HalfHeight, float MaxDamage)
{
	if (!HasAuthority() || !MapData || HalfWidth <= 0.0f || HalfHeight <= 0.0f || MaxDamage <= 0.0f)
	{
		return false;
	}

	const FGridMapConfig& Config = MapData->GetConfig();
	int32 CenterX = 0;
	int32 CenterY = 0;
	MapData->WorldToGrid(Center, CenterX, CenterY);
	const int32 GridHalfWidth = FMath::CeilToInt(HalfWidth / Config.TileSize);
	const int32 GridHalfHeight = FMath::CeilToInt(HalfHeight / Config.TileSize);
	const int32 DamageAmount = FMath::RoundToInt(MaxDamage);
	bool bAnyDamage = false;

	for (int32 GridY = FMath::Max(0, CenterY - GridHalfHeight);
		GridY <= FMath::Min(Config.MapHeight - 1, CenterY + GridHalfHeight); ++GridY)
	{
		for (int32 GridX = FMath::Max(0, CenterX - GridHalfWidth);
			GridX <= FMath::Min(Config.MapWidth - 1, CenterX + GridHalfWidth); ++GridX)
		{
			const FVector TileCenter = MapData->GridToWorld(GridX, GridY);
			if (FMath::Abs(TileCenter.X - Center.X) > HalfWidth ||
				FMath::Abs(TileCenter.Y - Center.Y) > HalfHeight)
			{
				continue;
			}

			const FGridTileData* Tile = MapData->GetTile(GridX, GridY);
			if (Tile && Tile->IsDestructible() && DamageAmount > 0)
			{
				DamageTile(GridX, GridY, DamageAmount);
				bAnyDamage = true;
			}
		}
	}

	return bAnyDamage;
}

bool AGridMapManager::TryBuildTileAtWorldLocation(AActor* RequestingActor,
	const FVector& WorldLocation, EGridTileType BuildType)
{
	const bool bValidBuildType = BuildType == EGridTileType::Tree ||
		BuildType == EGridTileType::Rock || BuildType == EGridTileType::LargeTree;
	if (!HasAuthority() || !MapData || !RequestingActor || !bValidBuildType ||
		FVector::DistSquared2D(RequestingActor->GetActorLocation(), WorldLocation) > FMath::Square(MaxBuildDistance))
	{
		return false;
	}

	int32 GridX = 0;
	int32 GridY = 0;
	if (!MapData->WorldToGrid(WorldLocation, GridX, GridY))
	{
		return false;
	}

	const FGridTileData* ExistingTile = MapData->GetTile(GridX, GridY);
	if (!ExistingTile || ExistingTile->TileType != EGridTileType::Empty)
	{
		return false;
	}

	const int32 MaxHealth = GetDefaultHealthForType(BuildType);
	CommitTileState(GridX, GridY, BuildType, MaxHealth, MaxHealth);
	return true;
}

void AGridMapManager::CommitTileState(int32 GridX, int32 GridY, EGridTileType TileType,
	int32 Health, int32 MaxHealth)
{
	if (!HasAuthority() || !MapData || !MapData->IsValidCoord(GridX, GridY))
	{
		return;
	}

	FGridTileDeltaItem* Delta = TileDeltas.Items.FindByPredicate(
		[GridX, GridY](const FGridTileDeltaItem& Item)
		{
			return Item.GridX == GridX && Item.GridY == GridY;
		});

	if (!Delta)
	{
		Delta = &TileDeltas.Items.AddDefaulted_GetRef();
		Delta->GridX = GridX;
		Delta->GridY = GridY;
	}

	Delta->TileType = TileType;
	Delta->Health = FMath::Clamp(Health, 0, FMath::Max(0, MaxHealth));
	Delta->MaxHealth = FMath::Max(0, MaxHealth);
	Delta->GenerationVersion = BaselineState.GenerationVersion;
	Delta->Revision = ++NextTileRevision;
	TileDeltas.MarkItemDirty(*Delta);

	ApplyTileState(*Delta);
	ForceNetUpdate();
}

void AGridMapManager::HandleReplicatedTileDelta(const FGridTileDeltaItem& Delta)
{
	if (!HasAuthority() && MapData && Delta.GenerationVersion == BaselineState.GenerationVersion)
	{
		ApplyTileState(Delta);
	}
}

void AGridMapManager::ApplyAllReplicatedDeltas()
{
	if (!MapData)
	{
		return;
	}

	for (const FGridTileDeltaItem& Delta : TileDeltas.Items)
	{
		if (Delta.GenerationVersion == BaselineState.GenerationVersion)
		{
			ApplyTileState(Delta);
		}
	}
}

void AGridMapManager::ApplyTileState(const FGridTileDeltaItem& Delta)
{
	FGridTileData* Tile = MapData ? MapData->GetTileMutable(Delta.GridX, Delta.GridY) : nullptr;
	if (!Tile)
	{
		return;
	}

	const EGridTileType PreviousType = Tile->TileType;
	const bool bTypeChanged = PreviousType != Delta.TileType;
	Tile->TileType = Delta.TileType;
	Tile->Health = Delta.Health;
	Tile->MaxHealth = Delta.MaxHealth;

	if (bTypeChanged && MapRenderer)
	{
		MapRenderer->RefreshTileInstance(MapData, Delta.GridX, Delta.GridY);
		MarkTileNavigationDirty(Delta.GridX, Delta.GridY);
	}

	OnTileChanged.Broadcast(Delta.GridX, Delta.GridY, Delta.TileType);
	if (PreviousType != EGridTileType::Empty && Delta.TileType == EGridTileType::Empty)
	{
		OnTileDestroyed.Broadcast(Delta.GridX, Delta.GridY);
	}
}

void AGridMapManager::MarkTileNavigationDirty(int32 GridX, int32 GridY)
{
	if (!HasAuthority() || !MapData)
	{
		return;
	}

	if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld()))
	{
		const FVector WorldPos = MapData->GridToWorld(GridX, GridY);
		const FVector Extent(MapData->GetConfig().TileSize);
		NavSys->AddDirtyArea(FBox(WorldPos - Extent, WorldPos + Extent), ENavigationDirtyFlag::All);
	}
}

int32 AGridMapManager::GetDefaultHealthForType(EGridTileType TileType) const
{
	const FGridMapConfig& Config = BaselineState.Config;
	switch (TileType)
	{
	case EGridTileType::Tree:
		return Config.TreeHealth;
	case EGridTileType::LargeTree:
		return Config.LargeTreeHealth;
	default:
		return 0;
	}
}

void AGridMapManager::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AGridMapManager, BaselineState);
	DOREPLIFETIME(AGridMapManager, TileDeltas);
}
