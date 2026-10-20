// 网格地图系统 - 服务器权威管理器实现

#include "GridMapManager.h"
#include "WeaponGameState.h"
#include "GridMapData.h"
#include "GridMapGenerator.h"
#include "GridMapGenerator_RoomGraph.h"
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
		// 任务种子接管：travel options（单人任务页 / 大厅房主设置）写入 GameState，
		// 此处在生成前覆写 RequestedSeed；0 = 沿用资产内默认。
		if (const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr)
		{
			const int32 MissionSeed = WeaponGameState->GetMissionSettings().Seed;
			if (MissionSeed != 0)
			{
				RequestedSeed = MissionSeed;
			}
		}
		GenerateMap();
	}
	else if (!HasAuthority() && BaselineState.bGenerated)
	{
		FString Error;
		if (BuildBaselineMap(BaselineState, &Error))
		{
			ApplyAllReplicatedDeltas();
		}
		else
		{
			UE_LOG(LogTemp, Error, TEXT("[GridMap] 客户端 BeginPlay Baseline 重放失败：%s"), *Error);
		}
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

	if (MapRenderer) { MapRenderer->FlushDirtyChunks(); }
	FlushNavigationDirtyChunks();
}


bool AGridMapManager::GetMarkerWorldLocations(const EGridMapMarkerType MarkerType, TArray<FVector>& OutLocations) const
{
	OutLocations.Reset();
	if (!HasAuthority() || !BaselineState.bGenerated || !MapData)
	{
		return false;
	}
	for (const FGridMapRecipeMarker& Marker : BaselineState.Recipe.Markers)
	{
		if (Marker.Type == static_cast<uint8>(MarkerType))
		{
			OutLocations.Add(MapData->GridToWorld(Marker.GridCoord.X, Marker.GridCoord.Y));
		}
	}
	return OutLocations.Num() > 0;
}
void AGridMapManager::GenerateMap()
{
	if (!HasAuthority())
	{
		return;
	}
	const double GenerationStartSeconds = FPlatformTime::Seconds();

	FGridMapBaselineState NewState;
	NewState.SchemaVersion = 2;
	NewState.GenerationVersion = BaselineState.GenerationVersion + 1;
	NewState.Config = MapConfig;
	NewState.GeneratorClass = GeneratorClass;
	NewState.EffectiveSeed = RequestedSeed;
	if (NewState.EffectiveSeed == 0 && GeneratorClass)
	{
		if (const UGridMapGenerator_Simple* SimpleDefaults = Cast<UGridMapGenerator_Simple>(GeneratorClass->GetDefaultObject()))
		{
			NewState.EffectiveSeed = SimpleDefaults->Seed;
		}
	}
	if (NewState.EffectiveSeed == 0)
	{
		NewState.EffectiveSeed = FMath::RandRange(1, MAX_int32);
	}
	FString Error;
	if (!BuildBaselineMap(NewState, &Error))
	{
		UE_LOG(LogTemp, Error, TEXT("[GridMap] 基础地图生成失败：%s"), *Error);
		return;
	}
	NewState.AlgorithmVersion = Generator->GetAlgorithmVersion();
	NewState.TemplateSetVersion = Generator->GetTemplateSetVersion();
	NewState.Recipe = Generator->GetGeneratedRecipe();
	NewState.ResultHash = UGridMapGenerator::ComputeStableResultHash(*MapData, NewState.EffectiveSeed,
		NewState.AlgorithmVersion, NewState.TemplateSetVersion, NewState.Recipe);
	NewState.bGenerated = true;
	BaselineState = NewState;
	TileDeltas.Items.Reset();
	TileDeltas.MarkArrayDirty();
	NextTileRevision = 0;

	ForceNetUpdate();
	const double GenerationElapsedMilliseconds = (FPlatformTime::Seconds() - GenerationStartSeconds) * 1000.0;
	const int32 ActiveChunkCount = MapRenderer ? MapRenderer->GetActiveChunkCount() : 0;
	const int32 HISMComponentCount = MapRenderer ? MapRenderer->GetHISMComponentCount() : 0;
	const int32 TreeInstanceCount = MapRenderer ? MapRenderer->GetInstanceCountForType(EGridTileType::Tree) : 0;
	const int32 RockInstanceCount = MapRenderer ? MapRenderer->GetInstanceCountForType(EGridTileType::Rock) : 0;
	const int32 LargeTreeInstanceCount = MapRenderer ? MapRenderer->GetInstanceCountForType(EGridTileType::LargeTree) : 0;
	UE_LOG(LogTemp, Log, TEXT("[GridMap][Stage4D] 服务器地图生成并发布成功：耗时=%.3fms，GenerationVersion=%d，EffectiveSeed=%d，AlgorithmVersion=%d，TemplateSetVersion=%d，Recipe房间=%d，Recipe连接=%d，ResultHash=%llu，Renderer活动区块=%d，HISM组件=%d，Tree实例=%d，Rock实例=%d，LargeTree实例=%d，总实例=%d。"),
		GenerationElapsedMilliseconds, NewState.GenerationVersion, NewState.EffectiveSeed, NewState.AlgorithmVersion,
		NewState.TemplateSetVersion, NewState.Recipe.Rooms.Num(), NewState.Recipe.Connections.Num(),
		static_cast<unsigned long long>(NewState.ResultHash), ActiveChunkCount, HISMComponentCount,
		TreeInstanceCount, RockInstanceCount, LargeTreeInstanceCount, TreeInstanceCount + RockInstanceCount + LargeTreeInstanceCount);
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
	OutState.AlgorithmVersion = BaselineState.AlgorithmVersion;
	OutState.TemplateSetVersion = BaselineState.TemplateSetVersion;
	OutState.Recipe = BaselineState.Recipe;
	OutState.ResultHash = BaselineState.ResultHash;
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

	if (!ValidateRecoveryState(OutState, OutError))
	{
		return false;
	}
	UE_LOG(LogTemp, Log, TEXT("[GridMap][Stage4D] Recovery状态捕获成功：Seed=%d，GenerationVersion=%d，AlgorithmVersion=%d，TemplateSetVersion=%d，Recipe房间=%d，Recipe连接=%d，Delta=%d，ResultHash=%llu。"),
		OutState.EffectiveSeed, OutState.GenerationVersion, OutState.AlgorithmVersion, OutState.TemplateSetVersion,
		OutState.Recipe.Rooms.Num(), OutState.Recipe.Connections.Num(), OutState.TileDeltas.Num(),
		static_cast<unsigned long long>(OutState.ResultHash));
	return true;
}

bool AGridMapManager::ValidateRecoveryState(const FGridMapRecoveryState& State, FString& OutError)
{
	OutError.Reset();
	const FGridMapConfig& Config = State.Config;
	const int64 TileCount = static_cast<int64>(Config.MapWidth) * Config.MapHeight;
	if (State.SchemaVersion != GridMapRecoveryStateVersion ||
		State.BaselineSchemaVersion != 2 || !State.bGenerated ||
		State.GenerationVersion <= 0 || !State.GeneratorClass.IsValid() ||
		Config.MapWidth <= 0 || Config.MapHeight <= 0 ||
		!FMath::IsFinite(Config.TileSize) || Config.TileSize <= 0.0f ||
		Config.TreeHealth <= 0 || Config.LargeTreeHealth <= 0 ||
		TileCount <= 0 || TileCount > MAX_int32 || State.TileDeltas.Num() > TileCount)
	{
		OutError = TEXT("GridMap recovery baseline is invalid");
		return false;
	}
	UClass* RecoveryGeneratorClass = State.GeneratorClass.TryLoadClass<UGridMapGenerator>();
	if (!RecoveryGeneratorClass || !RecoveryGeneratorClass->IsChildOf(UGridMapGenerator::StaticClass()))
	{
		OutError = TEXT("GridMap recovery generator class could not be loaded or is not a GridMap generator");
		return false;
	}
	const bool bRoomGraph = RecoveryGeneratorClass->IsChildOf(UGridMapGenerator_RoomGraph::StaticClass());
	if (State.EffectiveSeed == 0 || (bRoomGraph && ((State.AlgorithmVersion != 1 && State.AlgorithmVersion != 2) || State.TemplateSetVersion <= 0
		|| State.ResultHash == 0 || State.Recipe.Rooms.IsEmpty())))
	{
		OutError = TEXT("GridMap recovery deterministic baseline is incomplete");
		return false;
	}
	if (bRoomGraph)
	{
		TArray<FIntRect> RecipeBounds;
		for (int32 Index = 0; Index < State.Recipe.Rooms.Num(); ++Index)
		{
			const FGridMapRecipeRoom& Room = State.Recipe.Rooms[Index];
			if (!Room.TemplatePath.IsValid() || Room.TemplateVersion <= 0 || Room.TemplateContentHash == 0
				|| Room.Size.X <= 0 || Room.Size.Y <= 0 || Room.Origin.X < 0 || Room.Origin.Y < 0
				|| Room.Origin.X + Room.Size.X > Config.MapWidth || Room.Origin.Y + Room.Size.Y > Config.MapHeight
				|| !StaticEnum<EGridMapRoomPurpose>()->IsValidEnumValue(static_cast<int64>(Room.Purpose)))
			{
				OutError = TEXT("GridMap recovery Recipe room is invalid"); return false;
			}
			const FIntRect Bounds(Room.Origin, Room.Origin + Room.Size);
			for (const FIntRect& Existing : RecipeBounds)
			{
				if (Bounds.Intersect(Existing)) { OutError = TEXT("GridMap recovery Recipe rooms overlap"); return false; }
			}
			RecipeBounds.Add(Bounds);
		}
		for (const FGridMapRecipeConnection& Connection : State.Recipe.Connections)
		{
			if (!State.Recipe.Rooms.IsValidIndex(Connection.FromRoom) || !State.Recipe.Rooms.IsValidIndex(Connection.ToRoom)
				|| Connection.FromRoom == Connection.ToRoom || Connection.CorridorWidth < 1 || Connection.CorridorWidth > 5
				|| Connection.CorridorWidth % 2 == 0 || Connection.Points.Num() < 2)
			{
				OutError = TEXT("GridMap recovery Recipe connection is invalid"); return false;
			}
			for (int32 PointIndex = 0; PointIndex < Connection.Points.Num(); ++PointIndex)
			{
				const FIntPoint Point = Connection.Points[PointIndex];
				const int32 HalfWidth = Connection.CorridorWidth / 2;
				if (Point.X - HalfWidth < 0 || Point.Y - HalfWidth < 0
					|| Point.X + HalfWidth >= Config.MapWidth || Point.Y + HalfWidth >= Config.MapHeight
					|| (PointIndex > 0 && Point.X != Connection.Points[PointIndex - 1].X && Point.Y != Connection.Points[PointIndex - 1].Y))
				{
					OutError = TEXT("GridMap recovery Recipe corridor is invalid"); return false;
				}
			}
		}
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
	RestoredBaseline.AlgorithmVersion = State.AlgorithmVersion;
	RestoredBaseline.TemplateSetVersion = State.TemplateSetVersion;
	RestoredBaseline.Recipe = State.Recipe;
	RestoredBaseline.ResultHash = State.ResultHash;
	RestoredBaseline.bGenerated = State.bGenerated;

	if (!BuildBaselineMap(RestoredBaseline, &OutError))
	{
		return false;
	}
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

	ApplyAllReplicatedDeltas();
	ForceNetUpdate();
	UE_LOG(LogTemp, Log, TEXT("[GridMap][Stage4D] Recovery状态恢复成功：Seed=%d，GenerationVersion=%d，AlgorithmVersion=%d，TemplateSetVersion=%d，Recipe房间=%d，Recipe连接=%d，Delta=%d，ResultHash=%llu。"),
		State.EffectiveSeed, State.GenerationVersion, State.AlgorithmVersion, State.TemplateSetVersion,
		State.Recipe.Rooms.Num(), State.Recipe.Connections.Num(), State.TileDeltas.Num(),
		static_cast<unsigned long long>(State.ResultHash));
	return true;
}

bool AGridMapManager::BuildBaselineMap(const FGridMapBaselineState& State, FString* OutError)
{
	if (State.GenerationVersion <= 0 || State.SchemaVersion != 2)
	{
		if (OutError) { *OutError = TEXT("GridMap Baseline schema 不受支持"); }
		return false;
	}

	UGridMapData* NewMapData = NewObject<UGridMapData>(this);
	NewMapData->Initialize(State.Config);
	UGridMapGenerator* NewGenerator = State.GeneratorClass ? NewObject<UGridMapGenerator>(this, State.GeneratorClass) : nullptr;

	if (!NewGenerator)
	{
		if (OutError) { *OutError = TEXT("GridMap GeneratorClass 无效"); }
		return false;
	}
	NewGenerator->SetEffectiveSeed(State.EffectiveSeed);
	NewGenerator->SetReplayRecipe(State.Recipe);
	if (UGridMapGenerator_RoomGraph* RoomGraph = Cast<UGridMapGenerator_RoomGraph>(NewGenerator))
	{
		const bool bNewGeneration = State.Recipe.IsEmpty() && State.ResultHash == 0 && !State.bGenerated;
		if (bNewGeneration)
		{
			RoomGraph->AlgorithmVersion = UGridMapGenerator_RoomGraph::CurrentAlgorithmVersion;
		}
		else
		{
			if (State.AlgorithmVersion != 1 && State.AlgorithmVersion != 2)
			{
				if (OutError) { *OutError = TEXT("GridMap RoomGraph 算法版本不受支持"); }
				return false;
			}
			RoomGraph->AlgorithmVersion = State.AlgorithmVersion;
		}
	}
	if (!NewGenerator->GenerateChecked(NewMapData))
	{
		if (OutError) { *OutError = NewGenerator->GetLastError(); }
		return false;
	}
	if (State.ResultHash != 0)
	{
		const uint64 ActualHash = UGridMapGenerator::ComputeStableResultHash(*NewMapData, State.EffectiveSeed,
			State.AlgorithmVersion, State.TemplateSetVersion, NewGenerator->GetGeneratedRecipe());
		if (ActualHash != State.ResultHash)
		{
			if (OutError) { *OutError = TEXT("GridMap Recipe 重放 ResultHash 不一致"); }
			return false;
		}
	}
	MapData = NewMapData;
	Generator = NewGenerator;
	VisibleTiles.Init(false, MapData->GetTotalTiles());
	MarkVisionDirty();

	if (MapRenderer)
	{
		MapRenderer->BuildFromMapData(MapData);
	}

	const FGridMapGenerationRecipe& BuiltRecipe = NewGenerator->GetGeneratedRecipe();
	const int32 ActiveChunkCount = MapRenderer ? MapRenderer->GetActiveChunkCount() : 0;
	const int32 HISMComponentCount = MapRenderer ? MapRenderer->GetHISMComponentCount() : 0;
	const int32 TreeInstanceCount = MapRenderer ? MapRenderer->GetInstanceCountForType(EGridTileType::Tree) : 0;
	const int32 RockInstanceCount = MapRenderer ? MapRenderer->GetInstanceCountForType(EGridTileType::Rock) : 0;
	const int32 LargeTreeInstanceCount = MapRenderer ? MapRenderer->GetInstanceCountForType(EGridTileType::LargeTree) : 0;
	UE_LOG(LogTemp, Log, TEXT("[GridMap][Stage4D] Baseline构建完成：GenerationVersion=%d，Seed=%d，ResultHash=%llu，Recipe房间=%d，Recipe连接=%d，Renderer活动区块=%d，HISM组件=%d，Tree实例=%d，Rock实例=%d，LargeTree实例=%d，总实例=%d。"),
		State.GenerationVersion, State.EffectiveSeed, static_cast<unsigned long long>(State.ResultHash),
		BuiltRecipe.Rooms.Num(), BuiltRecipe.Connections.Num(), ActiveChunkCount, HISMComponentCount,
		TreeInstanceCount, RockInstanceCount, LargeTreeInstanceCount, TreeInstanceCount + RockInstanceCount + LargeTreeInstanceCount);
	return true;
}

void AGridMapManager::OnRep_BaselineState()
{
	FString Error;
	if (BuildBaselineMap(BaselineState, &Error))
	{
		ApplyAllReplicatedDeltas();
	}
	else
	{
		UE_LOG(LogTemp, Error, TEXT("[GridMap] 客户端 Baseline 重放失败：%s"), *Error);
	}
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
	if (!HasAuthority() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this) ||
		!MapData || DamageAmount <= 0)
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
	if (!HasAuthority() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this) || !MapData)
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
	if (!HasAuthority() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this) ||
		!MapData || Radius <= 0.0f || MaxDamage <= 0.0f)
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
	if (!HasAuthority() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this) ||
		!MapData || HalfWidth <= 0.0f || HalfHeight <= 0.0f || MaxDamage <= 0.0f)
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
		if (Source->GetWorld() != GetWorld() || !Source->IsRegistered() || !Source->bActive || Source->TeamId != LocalTeamId
			|| !FMath::IsFinite(Source->Range) || Source->Range <= 0.0f)
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
		if (Source->GetWorld() != GetWorld() || !Source->IsRegistered() || !Source->bActive || Source->TeamId != LocalTeamId
			|| !FMath::IsFinite(Source->Range) || Source->Range <= 0.0f)
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

		const int32 GridRadius = FMath::CeilToInt(Source->Range / Config.TileSize) + 1;
		const int32 MinX = FMath::Max(0, SourceX - GridRadius);
		const int32 MaxX = FMath::Min(Config.MapWidth - 1, SourceX + GridRadius);
		const int32 MinY = FMath::Max(0, SourceY - GridRadius);
		const int32 MaxY = FMath::Min(Config.MapHeight - 1, SourceY + GridRadius);
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
				if (DistanceSq > RangeSq)
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

bool AGridMapManager::TryBuildTileAtWorldLocation(AActor* RequestingActor,
	const FVector& WorldLocation, EGridTileType BuildType)
{
	const bool bValidBuildType = BuildType == EGridTileType::Tree ||
		BuildType == EGridTileType::Rock || BuildType == EGridTileType::LargeTree;
	if (!HasAuthority() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this) ||
		!MapData || !RequestingActor || !bValidBuildType ||
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
	if (MapRenderer) { MapRenderer->FlushDirtyChunks(); }
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

	if (bTypeChanged)
	{
		if (MapRenderer)
		{
			MapRenderer->MarkDirtyChunk(Delta.GridX, Delta.GridY);
		}
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
	NavigationDirtyTracker.MarkTile(GridX, GridY, HasAuthority() && MapData != nullptr);
}

void AGridMapManager::FlushNavigationDirtyChunks()
{
	if (!HasAuthority() || !MapData)
	{
		NavigationDirtyTracker.Flush(false, [](const FIntPoint&) {});
		return;
	}
	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(GetWorld());
	if (!NavSys)
	{
		return;
	}

	const float TileSize = MapData->GetConfig().TileSize;
	constexpr float NavigationDirtyHalfHeight = 10000.0f;
	const int32 FlushedChunkCount = NavigationDirtyTracker.Flush(true, [NavSys, TileSize](const FIntPoint& ChunkCoord)
	{
		const FVector Min(
			ChunkCoord.X * GridMapChunkSize * TileSize,
			ChunkCoord.Y * GridMapChunkSize * TileSize,
			-NavigationDirtyHalfHeight);
		const FVector Max(
			(ChunkCoord.X + 1) * GridMapChunkSize * TileSize,
			(ChunkCoord.Y + 1) * GridMapChunkSize * TileSize,
			NavigationDirtyHalfHeight);
		NavSys->AddDirtyArea(FBox(Min, Max), ENavigationDirtyFlag::All);
	});
	if (FlushedChunkCount > 0)
	{
		UE_LOG(LogTemp, Log, TEXT("[GridMap][Stage4D] 服务端导航Dirty区块已提交：区块数=%d。"), FlushedChunkCount);
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
