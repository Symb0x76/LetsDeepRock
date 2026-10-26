// Swarm 池化虫群：池 Actor 实现（服务器单 Tick 模拟）

#include "Swarm/SwarmPoolActor.h"

#include "AbilitySystemComponent.h"
#include "AbilitySystemGlobals.h"
#include "Components/SceneComponent.h"
#include "Attributes/WeaponCombatSet.h"
#include "Data/GameDataSubsystem.h"
#include "EnemyCharacter.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"
#include "GameplayEffect.h"
#include "GridMap/GridDamageHelper.h"
#include "GridMap/GridMapManager.h"
#include "Swarm/SwarmBugPawn.h"
#include "WeaponGameState.h"
#include "WeaponGameplayTags.h"
#include "WeaponPlayerState.h"
#include "ProfilingDebugging/CsvProfiler.h"
#include "Stats/Stats.h"

// stat Swarm 与 CSV（-csvprofile）共用的池 Tick 计时
DECLARE_STATS_GROUP(TEXT("Swarm"), STATGROUP_Swarm, STATCAT_Advanced);
DECLARE_CYCLE_STAT(TEXT("SwarmPoolTick"), STAT_SwarmPoolTick, STATGROUP_Swarm);
CSV_DEFINE_CATEGORY(Swarm, true);

namespace SwarmPoolConstants
{
	inline constexpr float SpawnScatterRadius = 150.f;
	inline constexpr float SpawnSearchRadius = 400.f;
	inline constexpr float EliteSearchRadius = 1500.f;
	/** 伤害查询时对空间哈希快照的额外余量（虫在本帧 Tick 后可能已移动） */
	inline constexpr float DamageQuerySlack = 100.f;
	/** AttackInterval 为 0 的原型（自爆虫）挖掘时使用的间隔，避免每帧挖掘 */
	inline constexpr float FallbackDigInterval = 1.f;
}

ASwarmPoolActor::ASwarmPoolActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickGroup = TG_PrePhysics;

	// 仅服务器存在，不参与复制
	bReplicates = false;

	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(false);
	CombatSet = CreateDefaultSubobject<UWeaponCombatSet>(TEXT("CombatSet"));

	DamageEffectClass = TSoftClassPtr<UGameplayEffect>(FSoftObjectPath(TEXT("/Game/Variant_Weapon/BluePrints/GE_Damage.GE_Damage_C")));

	FallbackConfig.MaxHealth = 30.f;
	FallbackConfig.MoveSpeed = 400.f;
	FallbackConfig.AttackDamage = 10.f;
}

UAbilitySystemComponent* ASwarmPoolActor::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

void ASwarmPoolActor::BeginPlay()
{
	Super::BeginPlay();
	AbilitySystemComponent->InitAbilityActorInfo(this, this);
}

void ASwarmPoolActor::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	for (ASwarmBugPawn* Bug : Bugs)
	{
		if (IsValid(Bug))
		{
			Bug->OnDamaged = nullptr;
			// 仅池 Actor 被显式销毁时一并销毁虫；关卡/世界卸载时由引擎统一清理
			if (EndPlayReason == EEndPlayReason::Destroyed)
			{
				Bug->Destroy();
			}
		}
	}
	Bugs.Reset();
	Super::EndPlay(EndPlayReason);
}

void ASwarmPoolActor::InitPool(const int32 Capacity)
{
	UWorld* World = GetWorld();
	if (!World || !HasAuthority() || Bugs.Num() > 0 || Capacity <= 0)
	{
		return;
	}

	GridMapManager = FGridDamageHelper::FindGridMapManager(this);
	LoadedDamageEffect = DamageEffectClass.LoadSynchronous();
	if (!LoadedDamageEffect)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Swarm] Damage effect %s not found; bug attacks deal no damage"), *DamageEffectClass.ToString());
	}

	Pool = FSwarmSlotPool(Capacity);
	SlotStates.SetNum(Capacity);
	Bugs.Reserve(Capacity);

	FActorSpawnParameters Params;
	Params.Owner = this;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	for (int32 Slot = 0; Slot < Capacity; ++Slot)
	{
		ASwarmBugPawn* Bug = World->SpawnActor<ASwarmBugPawn>(ASwarmBugPawn::StaticClass(), GetActorTransform(), Params);
		if (!Bug)
		{
			UE_LOG(LogTemp, Error, TEXT("[Swarm] Failed to spawn bug pawn %d/%d"), Slot, Capacity);
			Pool = FSwarmSlotPool(Slot);
			SlotStates.SetNum(Slot);
			break;
		}
		Bug->OnDamaged = [this](ASwarmBugPawn& DamagedBug, const FSwarmDamageEvent& Event)
		{
			HandleBugDamaged(DamagedBug, Event);
		};
		Bugs.Add(Bug);
	}

	UE_LOG(LogTemp, Log, TEXT("[Swarm] Pool initialized with %d bugs (grid map %s)"),
		Bugs.Num(), GridMapManager.IsValid() ? TEXT("found") : TEXT("missing"));
}

float ASwarmPoolActor::GetSimTime() const
{
	const UWorld* World = GetWorld();
	return World ? World->GetTimeSeconds() - PausedTime : 0.f;
}

bool ASwarmPoolActor::IsTileBlocked(const FVector2D& Point, const double Z) const
{
	const AGridMapManager* Grid = GridMapManager.Get();
	if (!Grid)
	{
		return false;
	}
	FGridTileData Tile;
	return Grid->GetTileAtWorldLocation(FVector(Point, Z), Tile) && Tile.TileType != EGridTileType::Empty;
}

const FEnemyConfigRow& ASwarmPoolActor::ResolveEnemyConfig(const FName Row)
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UGameDataSubsystem* GameData = GameInstance ? GameInstance->GetSubsystem<UGameDataSubsystem>() : nullptr;
	if (const FEnemyConfigRow* Config = GameData ? GameData->GetEnemyConfig(Row) : nullptr)
	{
		return *Config;
	}
	if (!WarnedMissingRows.Contains(Row))
	{
		WarnedMissingRows.Add(Row);
		UE_LOG(LogTemp, Warning, TEXT("[Swarm] Enemy config row %s missing, using fallback (HP 30, speed 400, damage 10)"), *Row.ToString());
	}
	return FallbackConfig;
}

int32 ASwarmPoolActor::SpawnBugs(const FName Row, const FVector& Location, const int32 Count)
{
	if (!HasAuthority() || Count <= 0)
	{
		return 0;
	}
	const FSwarmArchetypeTuning* Tuning = SwarmFindArchetypeTuning(Row);
	if (!Tuning)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Swarm] %s is not a swarm archetype"), *Row.ToString());
		return 0;
	}
	const FEnemyConfigRow& Config = ResolveEnemyConfig(Row);
	const float SimTime = GetSimTime();
	// 调用方给的 Z 可能是地面标记点，也可能是玩家胶囊中心；向下找地面，让碰撞柱底贴地。
	double GroundZ = Location.Z;
	FHitResult GroundHit;
	FCollisionObjectQueryParams GroundQuery(ECC_WorldStatic);
	if (GetWorld()->LineTraceSingleByObjectType(GroundHit, Location + FVector(0.0, 0.0, 50.0),
		Location - FVector(0.0, 0.0, 1000.0), GroundQuery))
	{
		GroundZ = GroundHit.ImpactPoint.Z;
	}
	const double Z = GroundZ + SwarmConstants::CollisionHalfHeight;
	auto IsBlocked = [this, Z](const FVector2D& Point) { return IsTileBlocked(Point, Z); };

	int32 Spawned = 0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FVector2D Desired = FVector2D(Location) + FMath::RandPointInCircle(SwarmPoolConstants::SpawnScatterRadius);
		FVector2D SpawnPoint;
		if (!SwarmFindFreeSpawnPoint(Desired, SwarmPoolConstants::SpawnSearchRadius, IsBlocked, SpawnPoint))
		{
			continue;
		}

		const int32 Slot = Pool.Acquire();
		if (Slot == INDEX_NONE)
		{
			break; // 池满
		}

		FSwarmSlotState& State = SlotStates[Slot];
		State = FSwarmSlotState();
		State.MoveSpeed = Config.MoveSpeed;
		State.AttackDamage = Config.AttackDamage;
		State.Tuning = Tuning;
		State.Config = &Config;
		State.NextRetargetTime = SimTime;
		State.NextAttackTime = SimTime;

		Bugs[Slot]->ActivateBug(Slot, Row, FVector(SpawnPoint, Z), Config.MaxHealth);
		SpatialHash.Insert(Slot, SpawnPoint);
		++Spawned;
	}
	return Spawned;
}

int32 ASwarmPoolActor::DamageInRadius(const FVector& Center, const float Radius, const FSwarmDamageEvent& Event)
{
	if (!HasAuthority() || Radius <= 0.f || AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return 0;
	}

	const FVector2D Center2D(Center);
	QueryScratch.Reset();
	SpatialHash.Query(Center2D, Radius + SwarmPoolConstants::DamageQuerySlack, QueryScratch);

	// 先收集再结算：结算可能回收槽位
	TArray<ASwarmBugPawn*, TInlineAllocator<64>> Hits;
	for (const int32 Slot : QueryScratch)
	{
		if (!Pool.IsActive(Slot))
		{
			continue;
		}
		ASwarmBugPawn* Bug = Bugs[Slot];
		const float Reach = Radius + SlotStates[Slot].Tuning->CollisionRadius;
		if (FVector2D::DistSquared(FVector2D(Bug->GetActorLocation()), Center2D) <= FMath::Square(Reach))
		{
			Hits.AddUnique(Bug); // 回收槽位可能在哈希快照里出现两次
		}
	}
	for (ASwarmBugPawn* Bug : Hits)
	{
		Bug->ReceiveSwarmDamage(Event);
	}
	return Hits.Num();
}

int32 ASwarmPoolActor::DamageFirstOnLine(const FVector& Start, const FVector& End, const FSwarmDamageEvent& Event)
{
	if (!HasAuthority() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return 0;
	}

	const FVector2D Start2D(Start);
	const FVector2D Segment = FVector2D(End) - Start2D;
	const double SegmentLengthSquared = Segment.SizeSquared();

	QueryScratch.Reset();
	const float QueryRadius = static_cast<float>(FMath::Sqrt(SegmentLengthSquared) * 0.5) + SwarmPoolConstants::DamageQuerySlack;
	SpatialHash.Query(Start2D + Segment * 0.5, QueryRadius, QueryScratch);

	ASwarmBugPawn* FirstHit = nullptr;
	double FirstHitAlpha = TNumericLimits<double>::Max();
	for (const int32 Slot : QueryScratch)
	{
		if (!Pool.IsActive(Slot))
		{
			continue;
		}
		ASwarmBugPawn* Bug = Bugs[Slot];
		const FVector2D BugPosition(Bug->GetActorLocation());
		const double Alpha = SegmentLengthSquared > UE_KINDA_SMALL_NUMBER
			? FMath::Clamp(FVector2D::DotProduct(BugPosition - Start2D, Segment) / SegmentLengthSquared, 0.0, 1.0)
			: 0.0;
		const FVector2D Closest = Start2D + Segment * Alpha;
		const float BugRadius = SlotStates[Slot].Tuning->CollisionRadius;
		if (FVector2D::DistSquared(BugPosition, Closest) <= FMath::Square(BugRadius) && Alpha < FirstHitAlpha)
		{
			FirstHit = Bug;
			FirstHitAlpha = Alpha;
		}
	}

	if (!FirstHit)
	{
		return 0;
	}
	FirstHit->ReceiveSwarmDamage(Event);
	return 1;
}

int32 ASwarmPoolActor::NumAlive() const
{
	return Pool.NumActive();
}

void ASwarmPoolActor::ForEachAliveView(TFunctionRef<void(const FSwarmBugView&)> Fn) const
{
	Pool.ForEachActive([this, &Fn](const int32 Slot)
	{
		const ASwarmBugPawn* Bug = Bugs[Slot];
		FSwarmBugView View;
		View.Location = Bug->GetActorLocation();
		View.ArchetypeRow = Bug->GetArchetypeRow();
		View.HealthFraction = Bug->GetMaxHealth() > 0.f ? Bug->GetHealth() / Bug->GetMaxHealth() : 0.f;
		Fn(View);
	});
}

void ASwarmPoolActor::DespawnAll()
{
	// 用局部数组:Tick 正在遍历成员 ActiveSlots 时也可能触发 DespawnAll。
	TArray<int32> SlotsToKill;
	Pool.ForEachActive([&SlotsToKill](const int32 Slot) { SlotsToKill.Add(Slot); });
	for (const int32 Slot : SlotsToKill)
	{
		KillSlot(Slot);
	}
	SpatialHash.Reset();
}

void ASwarmPoolActor::HandleBugDamaged(ASwarmBugPawn& Bug, const FSwarmDamageEvent& Event)
{
	if (AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}
	const int32 Slot = Bug.GetSlot();
	if (!Pool.IsActive(Slot))
	{
		return;
	}

	FSwarmSlotState& State = SlotStates[Slot];
	const ESwarmStatusFlags NewStatus = SwarmResolveStatus(Event.DamageTags, Event.StatusEffectId);
	if (NewStatus != ESwarmStatusFlags::None)
	{
		const float SimTime = GetSimTime();
		if (EnumHasAnyFlags(NewStatus, ESwarmStatusFlags::Burning))
		{
			State.BurnUntil = SimTime + SwarmConstants::BurnDuration;
		}
		if (EnumHasAnyFlags(NewStatus, ESwarmStatusFlags::Frozen))
		{
			State.FrozenUntil = SimTime + SwarmConstants::FrozenDuration;
		}
		Bug.SetStatusFlags(Bug.GetStatusFlags() | NewStatus);
	}

	ApplyHealthLoss(Slot, SwarmComputeDamage(Event.Damage, Event.DamageTags, *State.Config));
}

void ASwarmPoolActor::ApplyHealthLoss(const int32 Slot, const float Damage)
{
	if (!Pool.IsActive(Slot) || Damage <= 0.f)
	{
		return;
	}
	ASwarmBugPawn* Bug = Bugs[Slot];
	Bug->SetHealth(Bug->GetHealth() - Damage);
	if (Bug->GetHealth() <= 0.f)
	{
		KillSlot(Slot);
	}
}

void ASwarmPoolActor::KillSlot(const int32 Slot)
{
	if (!Pool.IsActive(Slot))
	{
		return;
	}
	Bugs[Slot]->DeactivateBug();
	Pool.Release(Slot);
	SlotStates[Slot] = FSwarmSlotState();
}

void ASwarmPoolActor::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	SCOPE_CYCLE_COUNTER(STAT_SwarmPoolTick);
	CSV_SCOPED_TIMING_STAT(Swarm, PoolTick);

	// 1. 恢复屏障：冻结模拟，并把暂停时长从 SimTime 中扣除
	if (AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		PausedTime += DeltaSeconds;
		return;
	}
	if (Pool.NumActive() == 0)
	{
		return;
	}
	const float SimTime = GetSimTime();

	// 2. 重建空间哈希；收集玩家、精英与障碍
	ActiveSlots.Reset();
	SlotPositions.SetNum(Bugs.Num(), EAllowShrinking::No);
	SpatialHash.Reset();
	Pool.ForEachActive([this](const int32 Slot)
	{
		const FVector2D Position(Bugs[Slot]->GetActorLocation());
		SlotPositions[Slot] = Position;
		SpatialHash.Insert(Slot, Position);
		ActiveSlots.Add(Slot);
	});
	GatherPlayersAndObstacles();

	// 3~5. 逐槽位模拟
	for (const int32 Slot : ActiveSlots)
	{
		if (Pool.IsActive(Slot))
		{
			SimulateSlot(Slot, DeltaSeconds, SimTime);
		}
	}
}

void ASwarmPoolActor::GatherPlayersAndObstacles()
{
	UWorld* World = GetWorld();
	Players.Reset();
	Obstacles.Reset();

	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		const APlayerController* Controller = It->Get();
		APawn* PlayerPawn = Controller ? Controller->GetPawn() : nullptr;
		if (!IsValid(PlayerPawn))
		{
			continue;
		}
		const AWeaponPlayerState* PlayerState = Controller->GetPlayerState<AWeaponPlayerState>();
		const UAbilitySystemComponent* PlayerASC = PlayerState ? PlayerState->GetAbilitySystemComponent() : nullptr;
		if (PlayerASC && PlayerASC->HasMatchingGameplayTag(TAG_State_Dead))
		{
			continue;
		}
		Players.Add(PlayerPawn);
		Obstacles.Add({FVector2D(PlayerPawn->GetActorLocation()), PlayerPawn->GetSimpleCollisionRadius()});
	}

	for (TActorIterator<AEnemyCharacter> It(World); It; ++It)
	{
		const AEnemyCharacter* Elite = *It;
		if (!IsValid(Elite))
		{
			continue;
		}
		const FVector2D ElitePosition(Elite->GetActorLocation());
		QueryScratch.Reset();
		SpatialHash.Query(ElitePosition, SwarmPoolConstants::EliteSearchRadius, QueryScratch);
		if (QueryScratch.Num() > 0)
		{
			Obstacles.Add({ElitePosition, Elite->GetSimpleCollisionRadius()});
		}
	}
}

void ASwarmPoolActor::SimulateSlot(const int32 Slot, const float DeltaSeconds, const float SimTime)
{
	ASwarmBugPawn* Bug = Bugs[Slot];
	FSwarmSlotState& State = SlotStates[Slot];
	const FSwarmArchetypeTuning& Tuning = *State.Tuning;
	const FVector OldLocation = Bug->GetActorLocation();
	const FVector2D Position(OldLocation);

	// 3a. 重新选目标：目标死亡/离开时立即重选
	APawn* Target = State.Target.Get();
	if (Target && !Players.Contains(Target))
	{
		Target = nullptr;
		State.Target.Reset();
		State.NextRetargetTime = SimTime;
	}
	if (SimTime >= State.NextRetargetTime)
	{
		State.NextRetargetTime = SimTime + SwarmConstants::RetargetInterval;
		Target = nullptr;
		double BestDistanceSquared = FMath::Square(SwarmConstants::TargetSearchRadius);
		for (APawn* Player : Players)
		{
			const double DistanceSquared = FVector2D::DistSquared(FVector2D(Player->GetActorLocation()), Position);
			if (DistanceSquared <= BestDistanceSquared)
			{
				BestDistanceSquared = DistanceSquared;
				Target = Player;
			}
		}
		State.Target = Target;
	}

	// 3b. 燃烧 DoT 与状态到期
	ESwarmStatusFlags Flags = Bug->GetStatusFlags();
	if (EnumHasAnyFlags(Flags, ESwarmStatusFlags::Burning))
	{
		if (SimTime < State.BurnUntil)
		{
			// 燃烧伤害按火抗结算，但不经过 SwarmResolveStatus（否则会无限续燃）
			const FGameplayTagContainer FireTags(TAG_Damage_Type_Fire.GetTag());
			ApplyHealthLoss(Slot, SwarmComputeDamage(SwarmConstants::BurnDps * DeltaSeconds, FireTags, *State.Config));
			if (!Pool.IsActive(Slot))
			{
				return;
			}
		}
		else
		{
			Flags &= ~ESwarmStatusFlags::Burning;
		}
	}
	if (EnumHasAnyFlags(Flags, ESwarmStatusFlags::Frozen) && SimTime >= State.FrozenUntil)
	{
		Flags &= ~ESwarmStatusFlags::Frozen;
	}
	Bug->SetStatusFlags(Flags);

	// 3c. 转向 + 地形阻挡
	const bool bFrozen = EnumHasAnyFlags(Flags, ESwarmStatusFlags::Frozen);
	const float Speed = State.MoveSpeed * (bFrozen ? SwarmConstants::FrozenSpeedScale : 1.f);

	NeighborScratch.Reset();
	QueryScratch.Reset();
	SpatialHash.Query(Position, SwarmConstants::SeparationRadius, QueryScratch);
	for (const int32 Neighbor : QueryScratch)
	{
		if (Neighbor != Slot)
		{
			NeighborScratch.Add(SlotPositions[Neighbor]);
		}
	}

	FSwarmSteeringInput Input;
	Input.Position = Position;
	Input.bHasTarget = Target != nullptr;
	Input.TargetPosition = Target ? FVector2D(Target->GetActorLocation()) : Position;
	Input.MaxSpeed = Speed;
	Input.DeltaSeconds = DeltaSeconds;
	Input.SelfRadius = Tuning.CollisionRadius;
	Input.Neighbors = NeighborScratch;
	Input.Obstacles = Obstacles;

	const double Z = OldLocation.Z;
	auto IsBlocked = [this, Z](const FVector2D& Point) { return IsTileBlocked(Point, Z); };
	const FSwarmSteeringResult Steering = SwarmComputeSteeringStep(Input, IsBlocked);

	const FVector NewLocation(Steering.NewPosition, Z);
	const FVector2D Moved = Steering.NewPosition - Position;
	const FRotator NewRotation = Moved.SizeSquared() > UE_KINDA_SMALL_NUMBER
		? FRotator(0.f, FMath::RadiansToDegrees(FMath::Atan2(Moved.Y, Moved.X)), 0.f)
		: Bug->GetActorRotation();
	Bug->SetActorLocationAndRotation(NewLocation, NewRotation, false, nullptr, ETeleportType::None);

	// 4. 被地形挡住：朝目标方向挖掘
	if (Steering.bBlockedByTerrain && Target && SimTime >= State.NextAttackTime)
	{
		const FVector2D DigDirection = (FVector2D(Target->GetActorLocation()) - Steering.NewPosition).GetSafeNormal();
		const FVector DigPoint(Steering.NewPosition + DigDirection * Tuning.CollisionRadius, Z);
		FGameplayTagContainer DigTags;
		DigTags.AddTag(TAG_Damage_Terrain);
		DigTags.AddTag(TAG_Damage_Type_Physical);
		FGridDamageHelper::TryApplyTerrainDamage(this, DigPoint, State.AttackDamage, DigTags);
		State.NextAttackTime = SimTime + (Tuning.AttackInterval > 0.f ? Tuning.AttackInterval : SwarmPoolConstants::FallbackDigInterval);
	}

	// 5. 攻击 / 自爆
	if (Target && SimTime >= State.NextAttackTime)
	{
		const float Reach = Tuning.AttackRange + Tuning.CollisionRadius + Target->GetSimpleCollisionRadius();
		if (FVector2D::DistSquared(FVector2D(Target->GetActorLocation()), Steering.NewPosition) <= FMath::Square(Reach))
		{
			if (Tuning.ExplodeRadius > 0.f)
			{
				const float ExplodeRadiusSquared = FMath::Square(Tuning.ExplodeRadius);
				for (APawn* Player : Players)
				{
					if (FVector2D::DistSquared(FVector2D(Player->GetActorLocation()), Steering.NewPosition) <= ExplodeRadiusSquared)
					{
						ApplyDamageEffectToPawn(Player, State.AttackDamage, TAG_Damage_Type_Explosion, NewLocation);
					}
				}
				KillSlot(Slot);
				return;
			}
			ApplyDamageEffectToPawn(Target, State.AttackDamage, TAG_Damage_Type_Physical, NewLocation);
			State.NextAttackTime = SimTime + Tuning.AttackInterval;
		}
	}
}

void ASwarmPoolActor::ApplyDamageEffectToPawn(APawn* TargetPawn, const float Damage, const FGameplayTag& DamageTypeTag, const FVector& BugLocation)
{
	UAbilitySystemComponent* TargetASC = UAbilitySystemGlobals::GetAbilitySystemComponentFromActor(TargetPawn);
	if (!TargetASC || !LoadedDamageEffect || Damage <= 0.f)
	{
		return;
	}
	// 本 tick 的玩家快照可能已过期:目标刚死亡则跳过。
	if (TargetASC->HasMatchingGameplayTag(TAG_State_Dead))
	{
		return;
	}

	FGameplayEffectContextHandle Context = AbilitySystemComponent->MakeEffectContext();
	Context.AddSourceObject(this);
	Context.AddOrigin(BugLocation);
	const FGameplayEffectSpecHandle SpecHandle = AbilitySystemComponent->MakeOutgoingSpec(LoadedDamageEffect, 1.f, Context);
	if (!SpecHandle.IsValid())
	{
		return;
	}
	SpecHandle.Data->SetSetByCallerMagnitude(TAG_Data_Damage, Damage);
	SpecHandle.Data->AddDynamicAssetTag(DamageTypeTag);
	AbilitySystemComponent->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);
}
