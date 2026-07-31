// 网格地图系统 - 服务器权威管理器实现

#include "GridMapManager.h"
#include "GridMapData.h"
#include "GridMapGenerator.h"
#include "GridMapRenderer.h"
#include "NavigationSystem.h"
#include "Net/UnrealNetwork.h"
#include "DrawDebugHelpers.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UObjectIterator.h"
#include "Variant_Weapon/Vision/VisionSourceComponent.h"
#include "Variant_Weapon/WeaponPlayerState.h"

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
	PrimaryActorTick.bCanEverTick = true;
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

void AGridMapManager::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);

	VisionUpdateAccumulator += DeltaSeconds;
	const bool bSampleNow = VisionUpdateAccumulator >= VisionUpdateInterval;
	if (bSampleNow)
	{
		VisionUpdateAccumulator = 0.0f;
		SampleVisionSourceState();
	}

	const bool bWasDirty = bVisionDirty;
	if (bVisionDirty)
	{
		RebuildLocalVisibility();
	}

	// 事件驱动重建后，调试图形仍按采样频率刷新自身的短持续时间。
	if (bDrawVisionDebug && (bSampleNow || bWasDirty))
	{
		DrawVisionDebug();
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

bool AGridMapManager::CaptureRecoveryState(FGridMapRecoveryState& OutState, FString& OutError) const
{
	OutState = FGridMapRecoveryState();
	OutError.Reset();
	if (!HasAuthority() || !BaselineState.bGenerated || !MapData)
	{
		OutError = TEXT("GridMap recovery state requires a generated Authority map");
		return false;
	}

	OutState.BaselineSchemaVersion = BaselineState.SchemaVersion;
	OutState.GenerationVersion = BaselineState.GenerationVersion;
	OutState.Config = BaselineState.Config;
	OutState.GeneratorClass = FSoftClassPath(BaselineState.GeneratorClass.Get());
	OutState.EffectiveSeed = BaselineState.EffectiveSeed;
	OutState.bGenerated = BaselineState.bGenerated;
	OutState.NextTileRevision = NextTileRevision;
	OutState.TileDeltas.Reserve(TileDeltas.Items.Num());

	for (const FGridTileDeltaItem& Delta : TileDeltas.Items)
	{
		FGridTileRecoveryState& SavedDelta = OutState.TileDeltas.AddDefaulted_GetRef();
		SavedDelta.GridX = Delta.GridX;
		SavedDelta.GridY = Delta.GridY;
		SavedDelta.TileType = Delta.TileType;
		SavedDelta.Health = Delta.Health;
		SavedDelta.MaxHealth = Delta.MaxHealth;
		SavedDelta.GenerationVersion = Delta.GenerationVersion;
		SavedDelta.Revision = Delta.Revision;
	}

	return ValidateRecoveryState(OutState, OutError);
}

bool AGridMapManager::ValidateRecoveryState(const FGridMapRecoveryState& State, FString& OutError)
{
	OutError.Reset();
	const FGridMapConfig& Config = State.Config;
	const int64 TileCount = static_cast<int64>(Config.MapWidth) * Config.MapHeight;
	if (State.SchemaVersion != GridMapRecoveryStateVersion ||
		State.BaselineSchemaVersion != 1 || !State.bGenerated ||
		State.GenerationVersion <= 0 || !State.GeneratorClass.IsValid() ||
		Config.MapWidth <= 0 || Config.MapHeight <= 0 ||
		!FMath::IsFinite(Config.TileSize) || Config.TileSize <= 0.0f ||
		Config.TreeHealth <= 0 || Config.LargeTreeHealth <= 0 ||
		TileCount <= 0 || TileCount > MAX_int32 || State.TileDeltas.Num() > TileCount)
	{
		OutError = TEXT("GridMap recovery baseline is invalid");
		return false;
	}

	TSet<int64> Coordinates;
	uint32 HighestRevision = 0;
	for (const FGridTileRecoveryState& Delta : State.TileDeltas)
	{
		const bool bValidTileType = StaticEnum<EGridTileType>()->IsValidEnumValue(
			static_cast<int64>(Delta.TileType));
		if (Delta.GridX < 0 || Delta.GridX >= Config.MapWidth ||
			Delta.GridY < 0 || Delta.GridY >= Config.MapHeight ||
			!bValidTileType || Delta.Health < 0 || Delta.MaxHealth < 0 ||
			Delta.Health > Delta.MaxHealth || Delta.GenerationVersion != State.GenerationVersion ||
			Delta.Revision == 0)
		{
			OutError = TEXT("GridMap recovery Tile Delta is invalid");
			return false;
		}

		const int64 Coordinate = static_cast<int64>(Delta.GridY) * Config.MapWidth + Delta.GridX;
		if (Coordinates.Contains(Coordinate))
		{
			OutError = TEXT("GridMap recovery contains duplicate Tile Delta coordinates");
			return false;
		}
		Coordinates.Add(Coordinate);
		HighestRevision = FMath::Max(HighestRevision, Delta.Revision);
	}

	if (State.NextTileRevision < HighestRevision)
	{
		OutError = TEXT("GridMap recovery revision is older than one of its Tile Deltas");
		return false;
	}
	return true;
}

bool AGridMapManager::RestoreRecoveryState(const FGridMapRecoveryState& State, FString& OutError)
{
	if (!HasAuthority() || !ValidateRecoveryState(State, OutError))
	{
		if (!HasAuthority())
		{
			OutError = TEXT("GridMap recovery can only run on Authority");
		}
		return false;
	}

	UClass* LoadedGeneratorClass = State.GeneratorClass.TryLoadClass<UGridMapGenerator>();
	if (!LoadedGeneratorClass)
	{
		OutError = FString::Printf(TEXT("GridMap generator class %s could not be loaded"),
			*State.GeneratorClass.ToString());
		return false;
	}

	FGridMapBaselineState RestoredBaseline;
	RestoredBaseline.SchemaVersion = State.BaselineSchemaVersion;
	RestoredBaseline.GenerationVersion = State.GenerationVersion;
	RestoredBaseline.Config = State.Config;
	RestoredBaseline.GeneratorClass = LoadedGeneratorClass;
	RestoredBaseline.EffectiveSeed = State.EffectiveSeed;
	RestoredBaseline.bGenerated = State.bGenerated;

	BaselineState = RestoredBaseline;
	MapConfig = State.Config;
	GeneratorClass = LoadedGeneratorClass;
	TileDeltas.Items.Reset(State.TileDeltas.Num());
	for (const FGridTileRecoveryState& SavedDelta : State.TileDeltas)
	{
		FGridTileDeltaItem& Delta = TileDeltas.Items.AddDefaulted_GetRef();
		Delta.GridX = SavedDelta.GridX;
		Delta.GridY = SavedDelta.GridY;
		Delta.TileType = SavedDelta.TileType;
		Delta.Health = SavedDelta.Health;
		Delta.MaxHealth = SavedDelta.MaxHealth;
		Delta.GenerationVersion = SavedDelta.GenerationVersion;
		Delta.Revision = SavedDelta.Revision;
	}
	TileDeltas.MarkArrayDirty();
	NextTileRevision = State.NextTileRevision;

	BuildBaselineMap(BaselineState);
	ApplyAllReplicatedDeltas();
	ForceNetUpdate();
	return true;
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
	VisibleTiles.Init(false, MapData->GetTotalTiles());
	MarkVisionDirty();

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

bool AGridMapManager::IsTileVisible(int32 GridX, int32 GridY) const
{
	if (!MapData || !MapData->IsValidCoord(GridX, GridY))
	{
		return false;
	}

	const int32 Index = GridY * MapData->GetConfig().MapWidth + GridX;
	return VisibleTiles.IsValidIndex(Index) && VisibleTiles[Index];
}

int32 AGridMapManager::GetVisibleTileCount() const
{
	int32 Count = 0;
	for (TConstSetBitIterator<> It(VisibleTiles); It; ++It)
	{
		++Count;
	}
	return Count;
}

void AGridMapManager::MarkVisionDirty()
{
	bVisionDirty = true;
}

void AGridMapManager::SampleVisionSourceState()
{
	const APlayerController* LocalPC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	const AWeaponPlayerState* LocalPS = LocalPC ? LocalPC->GetPlayerState<AWeaponPlayerState>() : nullptr;
	if (!LocalPC || !LocalPC->IsLocalController() || !LocalPS)
	{
		return;
	}

	const int32 LocalTeamId = LocalPS->GetTeamId();
	if (!bHasSampledLocalTeam || LastSampledLocalTeamId != LocalTeamId)
	{
		bHasSampledLocalTeam = true;
		LastSampledLocalTeamId = LocalTeamId;
		MarkVisionDirty();
	}

	TArray<FVisionSourceSnapshot> NewSnapshots;
	for (TObjectIterator<UVisionSourceComponent> It; It; ++It)
	{
		UVisionSourceComponent* Source = *It;
		if (Source->GetWorld() != GetWorld() || !Source->IsRegistered() || !Source->bActive || Source->TeamId != LocalTeamId)
		{
			continue;
		}

		FVisionSourceSnapshot& Snapshot = NewSnapshots.AddDefaulted_GetRef();
		Snapshot.Source = Source;
		Snapshot.Shape = static_cast<uint8>(Source->Shape);
		Snapshot.Range = Source->Range;
		Snapshot.HalfAngle = Source->HalfAngle;
		Snapshot.TeamId = Source->TeamId;
		if (MapData)
		{
			MapData->WorldToGrid(Source->GetVisionLocation(), Snapshot.GridX, Snapshot.GridY);
		}
		const FVector Direction = Source->GetVisionDirection();
		Snapshot.Direction = FVector2D(Direction.X, Direction.Y).GetSafeNormal();
	}

	bool bChanged = NewSnapshots.Num() != VisionSourceSnapshots.Num();
	const float DirectionDotThreshold = FMath::Cos(FMath::DegreesToRadians(VisionDirectionChangeDegrees));
	for (const FVisionSourceSnapshot& NewSnapshot : NewSnapshots)
	{
		const FVisionSourceSnapshot* OldSnapshot = VisionSourceSnapshots.FindByPredicate(
			[&NewSnapshot](const FVisionSourceSnapshot& Candidate)
			{
				return Candidate.Source == NewSnapshot.Source;
			});

		if (!OldSnapshot
			|| OldSnapshot->Shape != NewSnapshot.Shape
			|| OldSnapshot->Range != NewSnapshot.Range
			|| OldSnapshot->HalfAngle != NewSnapshot.HalfAngle
			|| OldSnapshot->TeamId != NewSnapshot.TeamId
			|| OldSnapshot->GridX != NewSnapshot.GridX
			|| OldSnapshot->GridY != NewSnapshot.GridY
			|| (NewSnapshot.Shape == static_cast<uint8>(EVisionSourceShape::Cone)
				&& (OldSnapshot->Direction.IsNearlyZero() != NewSnapshot.Direction.IsNearlyZero()
					|| (!NewSnapshot.Direction.IsNearlyZero()
						&& FVector2D::DotProduct(OldSnapshot->Direction, NewSnapshot.Direction) < DirectionDotThreshold))))
		{
			bChanged = true;
			break;
		}
	}

	if (bChanged)
	{
		// 仅在触发重建时更新基准，使小于阈值的连续旋转可以累计到约 2 度。
		VisionSourceSnapshots = MoveTemp(NewSnapshots);
		MarkVisionDirty();
	}
}

void AGridMapManager::RebuildLocalVisibility()
{
	if (!MapData || MapData->GetTotalTiles() == 0)
	{
		return;
	}

	const APlayerController* LocalPC = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	const AWeaponPlayerState* LocalPS = LocalPC ? LocalPC->GetPlayerState<AWeaponPlayerState>() : nullptr;
	if (!LocalPC || !LocalPC->IsLocalController() || !LocalPS)
	{
		// PlayerState 到达前保留 dirty，确保一旦可用就立即完成首次计算。
		return;
	}

	bVisionDirty = false;
	VisibleTiles.Init(false, MapData->GetTotalTiles());

	const int32 LocalTeamId = LocalPS->GetTeamId();
	const FGridMapConfig& Config = MapData->GetConfig();

	for (TObjectIterator<UVisionSourceComponent> It; It; ++It)
	{
		UVisionSourceComponent* Source = *It;
		if (Source->GetWorld() != GetWorld() || !Source->IsRegistered() || !Source->bActive || Source->TeamId != LocalTeamId)
		{
			continue;
		}

		const FVector SourceLocation = Source->GetVisionLocation();
		int32 SourceX = 0;
		int32 SourceY = 0;
		if (!MapData->WorldToGrid(SourceLocation, SourceX, SourceY))
		{
			continue;
		}

		const bool bUnlimited = Source->Range <= 0.0f;
		const int32 GridRadius = bUnlimited
			? FMath::Max(Config.MapWidth, Config.MapHeight)
			: FMath::CeilToInt(Source->Range / Config.TileSize) + 1;
		const int32 MinX = bUnlimited ? 0 : FMath::Max(0, SourceX - GridRadius);
		const int32 MaxX = bUnlimited ? Config.MapWidth - 1 : FMath::Min(Config.MapWidth - 1, SourceX + GridRadius);
		const int32 MinY = bUnlimited ? 0 : FMath::Max(0, SourceY - GridRadius);
		const int32 MaxY = bUnlimited ? Config.MapHeight - 1 : FMath::Min(Config.MapHeight - 1, SourceY + GridRadius);
		const float RangeSq = Source->Range * Source->Range;
		const float ConeThreshold = FMath::Cos(FMath::DegreesToRadians(Source->HalfAngle));
		FVector2D Forward(Source->GetVisionDirection().X, Source->GetVisionDirection().Y);
		Forward.Normalize();

		for (int32 Y = MinY; Y <= MaxY; ++Y)
		{
			for (int32 X = MinX; X <= MaxX; ++X)
			{
				const FVector TileCenter = MapData->GridToWorld(X, Y);
				const FVector2D ToTile(TileCenter.X - SourceLocation.X, TileCenter.Y - SourceLocation.Y);
				const float DistanceSq = ToTile.SizeSquared();
				if (!bUnlimited && DistanceSq > RangeSq)
				{
					continue;
				}

				if (Source->Shape == EVisionSourceShape::Cone && DistanceSq > KINDA_SMALL_NUMBER)
				{
					const float Dot = FVector2D::DotProduct(Forward, ToTile.GetSafeNormal());
					if (Dot < ConeThreshold)
					{
						continue;
					}
				}

				if (HasLineOfSight(SourceX, SourceY, X, Y))
				{
					VisibleTiles[Y * Config.MapWidth + X] = true;
				}
			}
		}
	}

}

bool AGridMapManager::HasLineOfSight(int32 SourceX, int32 SourceY, int32 TargetX, int32 TargetY) const
{
	if (SourceX == TargetX && SourceY == TargetY)
	{
		return true;
	}
	const FGridTileData* SourceTile = MapData->GetTile(SourceX, SourceY);
	if (SourceTile && SourceTile->BlocksVision())
	{
		return false;
	}

	auto IsBlockingBeforeTarget = [this, TargetX, TargetY](int32 X, int32 Y)
	{
		if (X == TargetX && Y == TargetY)
		{
			return false;
		}
		const FGridTileData* Tile = MapData->GetTile(X, Y);
		return Tile && Tile->BlocksVision();
	};

	int32 X = SourceX;
	int32 Y = SourceY;
	const int32 DeltaX = TargetX - SourceX;
	const int32 DeltaY = TargetY - SourceY;
	const int32 StepX = FMath::Sign(DeltaX);
	const int32 StepY = FMath::Sign(DeltaY);
	const int32 AbsX = FMath::Abs(DeltaX);
	const int32 AbsY = FMath::Abs(DeltaY);
	int32 CountX = 0;
	int32 CountY = 0;

	while (CountX < AbsX || CountY < AbsY)
	{
		const int32 DecisionX = (1 + 2 * CountX) * AbsY;
		const int32 DecisionY = (1 + 2 * CountY) * AbsX;
		if (DecisionX == DecisionY)
		{
			// 射线恰好穿过角点时，两侧接触格都属于 supercover。
			if (IsBlockingBeforeTarget(X + StepX, Y) || IsBlockingBeforeTarget(X, Y + StepY))
			{
				return false;
			}
			X += StepX;
			Y += StepY;
			++CountX;
			++CountY;
		}
		else if (DecisionX < DecisionY)
		{
			X += StepX;
			++CountX;
		}
		else
		{
			Y += StepY;
			++CountY;
		}

		if (IsBlockingBeforeTarget(X, Y))
		{
			return false;
		}
	}

	return true;
}

void AGridMapManager::DrawVisionDebug() const
{
	if (!MapData || MapData->GetTotalTiles() == 0)
	{
		return;
	}

	const float Duration = VisionUpdateInterval + 0.03f;
	const float TileSize = MapData->GetConfig().TileSize;
	int32 DrawnTiles = 0;
	for (TConstSetBitIterator<> It(VisibleTiles); It && DrawnTiles < MaxDebugDrawTiles; ++It, ++DrawnTiles)
	{
		const int32 Index = It.GetIndex();
		const int32 X = Index % MapData->GetConfig().MapWidth;
		const int32 Y = Index / MapData->GetConfig().MapWidth;
		DrawDebugBox(GetWorld(), MapData->GridToWorld(X, Y) + FVector(0.0f, 0.0f, 10.0f),
			FVector(TileSize * 0.45f, TileSize * 0.45f, 5.0f), FColor::Green, false, Duration, 0, 1.0f);
	}

	const APlayerController* LocalPC = GetWorld()->GetFirstPlayerController();
	const AWeaponPlayerState* LocalPS = LocalPC ? LocalPC->GetPlayerState<AWeaponPlayerState>() : nullptr;
	if (!LocalPC || !LocalPC->IsLocalController() || !LocalPS)
	{
		return;
	}

	for (TObjectIterator<UVisionSourceComponent> It; It; ++It)
	{
		const UVisionSourceComponent* Source = *It;
		if (Source->GetWorld() != GetWorld() || !Source->IsRegistered() || !Source->bActive || Source->TeamId != LocalPS->GetTeamId())
		{
			continue;
		}

		const FVector Location = Source->GetVisionLocation() + FVector(0.0f, 0.0f, 20.0f);
		const FGridMapConfig& Config = MapData->GetConfig();
		const float DebugRange = Source->Range > 0.0f
			? Source->Range
			: FMath::Max(Config.MapWidth, Config.MapHeight) * Config.TileSize;
		if (Source->Shape == EVisionSourceShape::Circle)
		{
			DrawDebugCircle(GetWorld(), Location, DebugRange, 64, FColor::Cyan, false, Duration, 0, 2.0f, FVector(1, 0, 0), FVector(0, 1, 0), false);
		}
		else
		{
			const FVector Direction = Source->GetVisionDirection().GetSafeNormal2D();
			const FVector Left = Direction.RotateAngleAxis(-Source->HalfAngle, FVector::UpVector);
			const FVector Right = Direction.RotateAngleAxis(Source->HalfAngle, FVector::UpVector);
			DrawDebugLine(GetWorld(), Location, Location + Left * DebugRange, FColor::Cyan, false, Duration, 0, 2.0f);
			DrawDebugLine(GetWorld(), Location, Location + Right * DebugRange, FColor::Cyan, false, Duration, 0, 2.0f);
		}
	}
}

void AGridMapManager::OnTileDestroyedInternal(int32 GridX, int32 GridY)
{
	MarkVisionDirty();
	// 移除渲染实例（同时移除碰撞，因为 HISM 碰撞和实例绑定）
	if (MapRenderer)
	{
		MapRenderer->RemoveTileInstance(GridX, GridY);
	}

	// 局部更新 NavMesh
	if (MapData)
	{
		const FVector WorldPos = MapData->GridToWorld(GridX, GridY);
		if (UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld()))
		{
			const FVector Extent(MapData->GetConfig().TileSize);
			NavSys->AddDirtyArea(FBox(WorldPos - Extent, WorldPos + Extent), ENavigationDirtyFlag::All);
		}
	}
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
	MarkVisionDirty();

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
