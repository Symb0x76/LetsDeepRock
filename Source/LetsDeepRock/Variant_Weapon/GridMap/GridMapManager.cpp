// 网格地图系统 - 管理器实现

#include "GridMapManager.h"
#include "GridMapData.h"
#include "GridMapGenerator.h"
#include "GridMapRenderer.h"
#include "NavigationSystem.h"
#include "DrawDebugHelpers.h"
#include "GameFramework/PlayerController.h"
#include "UObject/UObjectIterator.h"
#include "Variant_Weapon/Vision/VisionSourceComponent.h"
#include "Variant_Weapon/WeaponPlayerState.h"

AGridMapManager::AGridMapManager()
{
	PrimaryActorTick.bCanEverTick = true;

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
	// 1. 创建地图数据
	MapData = NewObject<UGridMapData>(this);
	MapData->Initialize(MapConfig);
	VisibleTiles.Init(false, MapData->GetTotalTiles());
	MarkVisionDirty();

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

bool AGridMapManager::DamageTilesInRadius(const FVector& Center, float Radius, float MaxDamage)
{
	if (!MapData || Radius <= 0.0f || MaxDamage <= 0.0f)
	{
		return false;
	}

	const FGridMapConfig& Config = MapData->GetConfig();

	// 将圆心和半径转换为网格坐标范围
	int32 CenterX, CenterY;
	MapData->WorldToGrid(Center, CenterX, CenterY);

	// 根据半径计算网格坐标偏移量，构建遍历的矩形边界
	const int32 GridRadius = FMath::CeilToInt(Radius / Config.TileSize) + 1;
	const int32 MinX = FMath::Max(0, CenterX - GridRadius);
	const int32 MaxX = FMath::Min(Config.MapWidth - 1, CenterX + GridRadius);
	const int32 MinY = FMath::Max(0, CenterY - GridRadius);
	const int32 MaxY = FMath::Min(Config.MapHeight - 1, CenterY + GridRadius);

	bool bAnyDamage = false;
	const float RadiusSq = Radius * Radius;
	const float InvRadius = 1.0f / Radius;

	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			// 计算格子中心到爆炸中心的距离
			const FVector TileCenter = MapData->GridToWorld(X, Y);
			FVector Delta = TileCenter - Center;
			Delta.Z = 0.0f; // 只考虑 2D 平面距离
			const float DistSq = Delta.SizeSquared();

			if (DistSq > RadiusSq)
			{
				continue; // 超出爆炸半径
			}

			// 线性距离衰减：MaxDamage at center → 0 at boundary
			const float Distance = FMath::Sqrt(DistSq);
			const float Attenuation = 1.0f - FMath::Clamp(Distance * InvRadius, 0.0f, 1.0f);
			const int32 ScaledDamage = FMath::RoundToInt(MaxDamage * Attenuation);

			if (ScaledDamage > 0)
			{
				if (DamageTile(X, Y, ScaledDamage))
				{
					bAnyDamage = true;
				}
			}
		}
	}

	return bAnyDamage;
}

bool AGridMapManager::DamageTilesInRect(const FVector& Center, float HalfWidth, float HalfHeight, float MaxDamage)
{
	if (!MapData || HalfWidth <= 0.0f || HalfHeight <= 0.0f || MaxDamage <= 0.0f)
	{
		return false;
	}

	const FGridMapConfig& Config = MapData->GetConfig();

	// 将矩形中心和半尺寸转换为网格坐标范围
	int32 CenterX, CenterY;
	MapData->WorldToGrid(Center, CenterX, CenterY);

	const int32 GridHalfWidth = FMath::CeilToInt(HalfWidth / Config.TileSize);
	const int32 GridHalfHeight = FMath::CeilToInt(HalfHeight / Config.TileSize);

	const int32 MinX = FMath::Max(0, CenterX - GridHalfWidth);
	const int32 MaxX = FMath::Min(Config.MapWidth - 1, CenterX + GridHalfWidth);
	const int32 MinY = FMath::Max(0, CenterY - GridHalfHeight);
	const int32 MaxY = FMath::Min(Config.MapHeight - 1, CenterY + GridHalfHeight);

	// 矩形世界坐标边界（用于精确判断格子中心是否在矩形内）
	const float RectMinX = Center.X - HalfWidth;
	const float RectMaxX = Center.X + HalfWidth;
	const float RectMinY = Center.Y - HalfHeight;
	const float RectMaxY = Center.Y + HalfHeight;

	bool bAnyDamage = false;
	const int32 IntDamage = FMath::RoundToInt(MaxDamage);

	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			const FVector TileCenter = MapData->GridToWorld(X, Y);

			// 检查格子中心是否在矩形内（仅 XY 平面）
			if (TileCenter.X < RectMinX || TileCenter.X > RectMaxX ||
				TileCenter.Y < RectMinY || TileCenter.Y > RectMaxY)
			{
				continue;
			}

			if (IntDamage > 0)
			{
				if (DamageTile(X, Y, IntDamage))
				{
					bAnyDamage = true;
				}
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
		const float DebugRange = Source->Range > 0.0f
			? Source->Range
			: FMath::Max(MapConfig.MapWidth, MapConfig.MapHeight) * MapConfig.TileSize;
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
