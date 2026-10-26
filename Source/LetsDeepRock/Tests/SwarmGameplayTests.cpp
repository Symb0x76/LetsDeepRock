// Swarm 池化虫群：关卡内玩法自动化测试（须以 -game 运行；打开 L_GridMapStage4Test 并驱动真实玩家/武器/手雷/GameMode 路径）

#include "Misc/AutomationTest.h"
#include "WeaponGameplayTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "UObject/StrongObjectPtr.h"
#include "UObject/UObjectGlobals.h"
#include "AI/EnemyAIController.h"
#include "AI/WeaponWaveDirectorComponent.h"
#include "Attributes/WeaponHealthSet.h"
#include "EnemyCharacter.h"
#include "GridMap/GridDamageHelper.h"
#include "GridMap/GridMapManager.h"
#include "Swarm/SwarmBackend.h"
#include "Swarm/SwarmBugPawn.h"
#include "Swarm/SwarmSubsystem.h"
#include "Swarm/SwarmTypes.h"
#include "WeaponCharacter.h"
#include "WeaponGameMode.h"
#include "WeaponGameState.h"
#include "WeaponPlayerState.h"
#include "Weapons/GrenadeBallistics.h"
#include "Weapons/GrenadeProjectile.h"
#include "Weapons/WeaponDataAsset.h"
#include "Weapons/WeaponEquipmentComponent.h"
#include "Weapons/WeaponGrenadeDataAsset.h"
#include "Weapons/WeaponProjectile.h"


static double SwarmGameplayDist2D(const FVector& A, const FVector& B)
{
	return FVector2D::Distance(FVector2D(A), FVector2D(B));
}

static float SwarmGameplayBugRadius(const FName Row)
{
	const FSwarmArchetypeTuning* Tuning = SwarmFindArchetypeTuning(Row);
	return Tuning ? Tuning->CollisionRadius : 0.f;
}

/** 当前世界里全部激活的池化虫 */
static void SwarmGameplayCollectActiveBugs(UWorld& World, TArray<ASwarmBugPawn*>& OutBugs)
{
	OutBugs.Reset();
	for (TActorIterator<ASwarmBugPawn> It(&World); It; ++It)
	{
		if (It->IsBugActive())
		{
			OutBugs.Add(*It);
		}
	}
}




/** 在开阔方向 Distance 处生成 Count 只 Row，并把当前全部激活虫记入 Bugs */
static bool SwarmGameplaySpawnAhead(FSwarmGameplayTestContext& Context, const FName Row, const float Distance, const int32 Count)
{
	const FVector Location = Context.Player->GetActorLocation() + Context.OpenDirection * Distance;
	const int32 Spawned = Context.GetBackend()->SpawnAt(Row, Location, Count);
	if (Spawned != Count)
	{
		Context.Fail(FString::Printf(TEXT("[Swarm] SpawnAt %s spawned %d/%d"), *Row.ToString(), Spawned, Count));
		return false;
	}
	TArray<ASwarmBugPawn*> Active;
	SwarmGameplayCollectActiveBugs(*Context.World, Active);
	Context.Bugs.Reset();
	for (ASwarmBugPawn* Bug : Active)
	{
		Context.Bugs.Add(Bug);
	}
	return true;
}

static double SwarmGameplayMeanDistanceToPlayer(const FSwarmGameplayTestContext& Context)
{
	double Sum = 0.0;
	int32 Num = 0;
	const FVector PlayerLocation = Context.Player->GetActorLocation();
	for (const TWeakObjectPtr<ASwarmBugPawn>& Bug : Context.Bugs)
	{
		if (Bug.IsValid() && Bug->IsBugActive())
		{
			Sum += SwarmGameplayDist2D(Bug->GetActorLocation(), PlayerLocation);
			++Num;
		}
	}
	return Num > 0 ? Sum / Num : 0.0;
}

/** 被跟踪虫的生命总和（已死亡/回收的计 0） */
static double SwarmGameplaySumTrackedHealth(const FSwarmGameplayTestContext& Context)
{
	double Sum = 0.0;
	for (const TWeakObjectPtr<ASwarmBugPawn>& Bug : Context.Bugs)
	{
		if (Bug.IsValid() && Bug->IsBugActive())
		{
			Sum += Bug->GetHealth();
		}
	}
	return Sum;
}

static int32 SwarmGameplayCountTrackedAlive(const FSwarmGameplayTestContext& Context)
{
	int32 Num = 0;
	for (const TWeakObjectPtr<ASwarmBugPawn>& Bug : Context.Bugs)
	{
		Num += Bug.IsValid() && Bug->IsBugActive() ? 1 : 0;
	}
	return Num;
}


#define SWARM_GAMEPLAY_TEST(ClassName, Path) IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, Path, EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

// ---------------------------------------------------------------------------
// 追击并停在玩家胶囊外
// ---------------------------------------------------------------------------
SWARM_GAMEPLAY_TEST(FSwarmGameplayChaseAndStopAtCapsuleTest, "LetsDeepRock.Swarm.Gameplay.ChaseAndStopAtCapsule")
bool FSwarmGameplayChaseAndStopAtCapsuleTest::RunTest(const FString& Parameters)
{
	constexpr int32 BugCount = 20;
	constexpr float SpawnDistance = 800.f;
	constexpr float ChaseSeconds = 4.f;
	constexpr double MinMeanApproach = 300.0;
	constexpr double ContactTolerance = 2.0;

	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, SpawnDistance + 200.f);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		if (SwarmGameplaySpawnAhead(Ctx, SwarmGameplayTestConstants::GruntRow, SpawnDistance, BugCount))
		{
			Ctx.ValueA = SwarmGameplayMeanDistanceToPlayer(Ctx);
		}
	});
	SwarmGameplayAddWorldWait(Context, ChaseSeconds);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		const double FinalMean = SwarmGameplayMeanDistanceToPlayer(Ctx);
		Ctx.Test->TestEqual(TEXT("all grunts still alive"), SwarmGameplayCountTrackedAlive(Ctx), BugCount);
		SwarmGameplayCheck(Ctx, FString::Printf(TEXT("mean 2D distance dropped >= %.0f (%.1f -> %.1f)"), MinMeanApproach, Ctx.ValueA, FinalMean),
			Ctx.ValueA - FinalMean >= MinMeanApproach);

		const double MinAllowed = Ctx.Player->GetCapsuleComponent()->GetScaledCapsuleRadius()
			+ SwarmGameplayBugRadius(SwarmGameplayTestConstants::GruntRow) - ContactTolerance;
		const FVector PlayerLocation = Ctx.Player->GetActorLocation();
		double Closest = TNumericLimits<double>::Max();
		for (const TWeakObjectPtr<ASwarmBugPawn>& Bug : Ctx.Bugs)
		{
			if (Bug.IsValid() && Bug->IsBugActive())
			{
				Closest = FMath::Min(Closest, SwarmGameplayDist2D(Bug->GetActorLocation(), PlayerLocation));
			}
		}
		SwarmGameplayCheck(Ctx, FString::Printf(TEXT("closest bug 2D distance %.1f >= %.1f"), Closest, MinAllowed), Closest >= MinAllowed);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// 虫阻挡玩家胶囊扫掠
// ---------------------------------------------------------------------------
SWARM_GAMEPLAY_TEST(FSwarmGameplayBugBlocksPlayerTest, "LetsDeepRock.Swarm.Gameplay.BugBlocksPlayer")
bool FSwarmGameplayBugBlocksPlayerTest::RunTest(const FString& Parameters)
{
	constexpr float SpawnDistance = 150.f;
	constexpr float SettleSeconds = 1.5f;
	constexpr double PushThrough = 200.0;
	constexpr double ContactTolerance = 2.0;

	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, SpawnDistance + PushThrough + 200.f);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		SwarmGameplaySpawnAhead(Ctx, SwarmGameplayTestConstants::GruntRow, SpawnDistance, 1);
	});
	SwarmGameplayAddWorldWait(Context, SettleSeconds);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		ASwarmBugPawn* Bug = Ctx.Bugs.Num() == 1 ? Ctx.Bugs[0].Get() : nullptr;
		if (!Bug || !Bug->IsBugActive())
		{
			Ctx.Fail(TEXT("[Swarm] tracked bug missing before sweep"));
			return;
		}
		AWeaponCharacter* Player = Ctx.Player.Get();
		const FVector Start = Player->GetActorLocation();
		const FVector ToBug = (Bug->GetActorLocation() - Start).GetSafeNormal2D();
		const FVector Target = Start + ToBug * (SwarmGameplayDist2D(Bug->GetActorLocation(), Start) + PushThrough);

		FHitResult Hit;
		Player->SetActorLocation(Target, true, &Hit, ETeleportType::None);
		Ctx.Test->TestTrue(TEXT("player sweep blocked"), Hit.bBlockingHit);
		SwarmGameplayCheck(Ctx, FString::Printf(TEXT("sweep blocked by ASwarmBugPawn (got %s)"), *GetNameSafe(Hit.GetActor())),
			Cast<ASwarmBugPawn>(Hit.GetActor()) == Bug);

		const double MinAllowed = Player->GetCapsuleComponent()->GetScaledCapsuleRadius()
			+ SwarmGameplayBugRadius(SwarmGameplayTestConstants::GruntRow) - ContactTolerance;
		const double After = SwarmGameplayDist2D(Bug->GetActorLocation(), Player->GetActorLocation());
		SwarmGameplayCheck(Ctx, FString::Printf(TEXT("player did not pass into bug (%.1f >= %.1f)"), After, MinAllowed), After >= MinAllowed);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// 真实射击能力（GA_Fire）从枪口高度命中虫
// ---------------------------------------------------------------------------
SWARM_GAMEPLAY_TEST(FSwarmGameplayProjectileHitsAtMuzzleHeightTest, "LetsDeepRock.Swarm.Gameplay.ProjectileHitsAtMuzzleHeight")
bool FSwarmGameplayProjectileHitsAtMuzzleHeightTest::RunTest(const FString& Parameters)
{
	constexpr float SpawnDistance = 600.f;
	constexpr float WeaponReadySeconds = 5.f;
	constexpr float HitWindowSeconds = 1.5f;
	/** 一击致死时，投射物最后观测位置与虫最后位置的最大 2D 距离（一帧飞行距离 + 碰撞半径的宽松上限） */
	constexpr double ProjectileKillProximity = 250.0;

	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, SpawnDistance + 200.f);

	// 等待默认武器装备完成
	SwarmGameplayAddWorldWait(Context, WeaponReadySeconds, [](FSwarmGameplayTestContext& Ctx, float)
	{
		const UWeaponEquipmentComponent* Equipment = Ctx.Player->FindComponentByClass<UWeaponEquipmentComponent>();
		const UWeaponDataAsset* Weapon = Equipment ? Equipment->GetCurrentWeaponData() : nullptr;
		Ctx.bFlagB = Weapon && Weapon->FireAbilityClass && Weapon->ProjectileClass && Equipment->GetCurrentMagazineAmmo() > 0;
		return !Ctx.bFlagB;
	});
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		if (!Ctx.bFlagB)
		{
			Ctx.Fail(TEXT("[Swarm] player has no projectile weapon with ammo equipped"));
			return;
		}
		if (!SwarmGameplaySpawnAhead(Ctx, SwarmGameplayTestConstants::GruntRow, SpawnDistance, 1))
		{
			return;
		}
		ASwarmBugPawn* Bug = Ctx.Bugs[0].Get();
		Ctx.ValueA = Bug->GetHealth();

		AWeaponCharacter* Player = Ctx.Player.Get();
		Player->SetAimDirectionAuthoritative(Bug->GetActorLocation() - Player->GetActorLocation());

		TSet<AWeaponProjectile*> Before;
		for (TActorIterator<AWeaponProjectile> It(Ctx.World.Get()); It; ++It)
		{
			Before.Add(*It);
		}
		const UWeaponDataAsset* Weapon = Player->FindComponentByClass<UWeaponEquipmentComponent>()->GetCurrentWeaponData();
		const bool bActivated = Ctx.PlayerState->GetAbilitySystemComponent()->TryActivateAbilityByClass(Weapon->FireAbilityClass);
		if (!bActivated)
		{
			Ctx.Fail(TEXT("[Swarm] fire ability failed to activate"));
			return;
		}
		AWeaponProjectile* Projectile = nullptr;
		for (TActorIterator<AWeaponProjectile> It(Ctx.World.Get()); It; ++It)
		{
			if (!Before.Contains(*It))
			{
				Projectile = *It;
				break;
			}
		}
		if (!Projectile)
		{
			Ctx.Fail(TEXT("[Swarm] fire ability spawned no projectile"));
			return;
		}
		const double BugBottom = Bug->GetActorLocation().Z - SwarmConstants::CollisionHalfHeight;
		const double BugTop = Bug->GetActorLocation().Z + SwarmConstants::CollisionHalfHeight;
		const double MuzzleZ = Projectile->GetActorLocation().Z;
		Ctx.Test->AddInfo(FString::Printf(TEXT("[Swarm] muzzle Z %.1f, bug column [%.1f, %.1f]"), MuzzleZ, BugBottom, BugTop));
		Ctx.Test->TestTrue(TEXT("projectile spawned within bug column height"), MuzzleZ >= BugBottom && MuzzleZ <= BugTop);

		// 命中判定数据：激活期间的最低生命、虫/投射物最后位置
		Ctx.TrackedActor = Projectile;
		Ctx.ValueC = Ctx.ValueA;
		Ctx.Positions = {Bug->GetActorLocation(), Projectile->GetActorLocation()};
		Ctx.bFlagB = false; // 此前用作“武器就绪”，此后表示“一击致死且投射物在侧”
	});
	SwarmGameplayAddWorldWait(Context, HitWindowSeconds, [](FSwarmGameplayTestContext& Ctx, float)
	{
		const ASwarmBugPawn* Bug = Ctx.Bugs[0].Get();
		const AActor* Projectile = Ctx.TrackedActor.Get();
		if (Bug && Bug->IsBugActive())
		{
			// 生命被扣但未死：激活期间直接观察到
			Ctx.ValueC = FMath::Min(Ctx.ValueC, static_cast<double>(Bug->GetHealth()));
			Ctx.Positions[0] = Bug->GetActorLocation();
			if (Projectile)
			{
				Ctx.Positions[1] = Projectile->GetActorLocation();
			}
			Ctx.bFlagA = Ctx.ValueC < Ctx.ValueA;
			return !Ctx.bFlagA;
		}
		// 一击致死：虫失活时，观测到的投射物须在虫最后位置附近（同帧命中）
		const FVector LastProjectile = Projectile ? Projectile->GetActorLocation() : Ctx.Positions[1];
		Ctx.ValueB = FMath::Min(SwarmGameplayDist2D(LastProjectile, Ctx.Positions[0]), SwarmGameplayDist2D(Ctx.Positions[1], Ctx.Positions[0]));
		Ctx.bFlagB = Ctx.ValueB <= ProjectileKillProximity;
		Ctx.bFlagA = Ctx.bFlagB;
		Ctx.Note = FString::Printf(TEXT("bug went inactive with projectile %.1f uu from it"), Ctx.ValueB);
		return false;
	});
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		const FString How = Ctx.bFlagB ? Ctx.Note : FString::Printf(TEXT("min health while active %.1f of %.1f; %s"), Ctx.ValueC, Ctx.ValueA, *Ctx.Note);
		SwarmGameplayCheck(Ctx, FString::Printf(TEXT("projectile damaged the bug, or killed it on contact, within 1.5 s (%s)"), *How), Ctx.bFlagA);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// 手雷（DA_Grenade_Default，与职业无关）伤害虫群
// ---------------------------------------------------------------------------
SWARM_GAMEPLAY_TEST(FSwarmGameplayGrenadeDamagesBugsTest, "LetsDeepRock.Swarm.Gameplay.GrenadeDamagesBugs")
bool FSwarmGameplayGrenadeDamagesBugsTest::RunTest(const FString& Parameters)
{
	constexpr int32 BugCount = 10;
	constexpr float SpawnDistance = 500.f;
	constexpr float SettleSeconds = 2.5f;
	constexpr float PostFuseSeconds = 0.5f;

	UWeaponGrenadeDataAsset* GrenadeData = LoadObject<UWeaponGrenadeDataAsset>(nullptr, SwarmGameplayTestConstants::GrenadeDataPath);
	if (!TestNotNull(TEXT("DA_Grenade_Default"), GrenadeData) || !TestNotNull(TEXT("grenade projectile class"), GrenadeData->ProjectileClass.Get()))
	{
		return false;
	}

	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, SpawnDistance + 200.f);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		SwarmGameplaySpawnAhead(Ctx, SwarmGameplayTestConstants::GruntRow, SpawnDistance, BugCount);
	});
	// 先让虫群追上玩家并停稳，再向其当前质心投掷，确保落点在群内
	SwarmGameplayAddWorldWait(Context, SettleSeconds);
	SwarmGameplayAddStep(Context, [GrenadeData = TStrongObjectPtr<UWeaponGrenadeDataAsset>(GrenadeData)](FSwarmGameplayTestContext& Ctx)
	{
		FVector Centroid = FVector::ZeroVector;
		int32 Num = 0;
		for (const TWeakObjectPtr<ASwarmBugPawn>& Bug : Ctx.Bugs)
		{
			if (Bug.IsValid() && Bug->IsBugActive())
			{
				Centroid += Bug->GetActorLocation();
				++Num;
			}
		}
		if (Num == 0)
		{
			Ctx.Fail(TEXT("[Swarm] grenade cluster vanished before throw"));
			return;
		}
		Centroid /= Num;
		const FVector TargetPoint = Centroid - FVector(0.0, 0.0, SwarmConstants::CollisionHalfHeight);
		Ctx.CountA = SwarmGameplayCountTrackedAlive(Ctx);
		Ctx.ValueA = SwarmGameplaySumTrackedHealth(Ctx);

		// 与 GA_ThrowGrenade 相同的生成与弹道解算
		AWeaponCharacter* Player = Ctx.Player.Get();
		FVector2D AimDirection2D = FVector2D(TargetPoint - Player->GetActorLocation()).GetSafeNormal();
		if (AimDirection2D.IsNearlyZero())
		{
			AimDirection2D = FVector2D(Ctx.OpenDirection);
		}
		const FVector Forward(AimDirection2D, 0.0);
		const FVector SpawnLocation = Player->GetActorLocation() + Forward * GrenadeData->SpawnForwardOffset
			+ FVector(0.0, 0.0, GrenadeData->SpawnUpOffset);
		const FGrenadeBallisticsInput Input = FGrenadeBallisticsInput::FromDataAsset(GrenadeData.Get(), SpawnLocation, AimDirection2D, TargetPoint, true, Player);
		FGrenadeBallisticsOutput Ballistics;
		FGrenadeBallistics::Calculate(Input, Ctx.World.Get(), Ballistics);

		const FTransform SpawnTransform(FRotator(GrenadeData->LaunchAngleDeg, Forward.Rotation().Yaw, 0.0), SpawnLocation);
		AGrenadeProjectile* Grenade = Ctx.World->SpawnActorDeferred<AGrenadeProjectile>(GrenadeData->ProjectileClass,
			FTransform::Identity, Player, Player, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Grenade)
		{
			Ctx.Fail(TEXT("[Swarm] grenade spawn failed"));
			return;
		}
		Grenade->InitFromGrenadeData(GrenadeData.Get(), Player);
		Grenade->FinishSpawning(SpawnTransform);
		Grenade->SetInitialThrowVelocity(Ballistics.InitialVelocity);
		Ctx.SpawnedActors.Add(Grenade);
		Ctx.Test->AddInfo(FString::Printf(TEXT("[Swarm] grenade target %s, predicted explosion %s"),
			*TargetPoint.ToCompactString(), *Ballistics.ExplosionPoint.ToCompactString()));
	});
	SwarmGameplayAddWorldWait(Context, GrenadeData->FuseSeconds + PostFuseSeconds);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		const int32 AliveAfter = SwarmGameplayCountTrackedAlive(Ctx);
		const double HealthAfter = SwarmGameplaySumTrackedHealth(Ctx);
		SwarmGameplayCheck(Ctx, FString::Printf(TEXT("grenade hurt the cluster (alive %d -> %d, health %.1f -> %.1f)"),
			Ctx.CountA, AliveAfter, Ctx.ValueA, HealthAfter), AliveAfter < Ctx.CountA || HealthAfter < Ctx.ValueA);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// 自爆虫伤害玩家并消失
// ---------------------------------------------------------------------------
SWARM_GAMEPLAY_TEST(FSwarmGameplayExploderHurtsPlayerTest, "LetsDeepRock.Swarm.Gameplay.ExploderHurtsPlayer")
bool FSwarmGameplayExploderHurtsPlayerTest::RunTest(const FString& Parameters)
{
	constexpr float SpawnDistance = 300.f;
	constexpr float MaxWaitSeconds = 6.f;

	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, SpawnDistance + 200.f);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		// 去掉护盾，让爆炸伤害直接落到 Health 上
		UAbilitySystemComponent* ASC = Ctx.PlayerState->GetAbilitySystemComponent();
		ASC->SetNumericAttributeBase(UWeaponHealthSet::GetMaxShieldAttribute(), 0.f);
		ASC->SetNumericAttributeBase(UWeaponHealthSet::GetShieldAttribute(), 0.f);
		Ctx.ValueA = Ctx.PlayerState->GetHealthSet()->GetHealth();
		Ctx.ValueB = Ctx.ValueA;
		SwarmGameplaySpawnAhead(Ctx, SwarmGameplayTestConstants::ExploderRow, SpawnDistance, 1);
	});
	SwarmGameplayAddWorldWait(Context, MaxWaitSeconds, [](FSwarmGameplayTestContext& Ctx, float)
	{
		Ctx.ValueB = FMath::Min(Ctx.ValueB, static_cast<double>(Ctx.PlayerState->GetHealthSet()->GetHealth()));
		return !(SwarmGameplayCountTrackedAlive(Ctx) == 0 && Ctx.ValueB < Ctx.ValueA);
	});
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		int32 ExplodersAlive = 0;
		Ctx.GetBackend()->ForEachAlive([&ExplodersAlive](const FSwarmBugView& View)
		{
			ExplodersAlive += View.ArchetypeRow == SwarmGameplayTestConstants::ExploderRow ? 1 : 0;
		});
		SwarmGameplayCheck(Ctx, FString::Printf(TEXT("player health dropped (%.1f -> %.1f)"), Ctx.ValueA, Ctx.ValueB), Ctx.ValueB < Ctx.ValueA);
		Ctx.Test->TestEqual(TEXT("exploder no longer alive"), ExplodersAlive, 0);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// 恢复屏障（GameMode 生产路径）冻结虫群，释放后恢复
// ---------------------------------------------------------------------------
SWARM_GAMEPLAY_TEST(FSwarmGameplayRecoveryBarrierFreezesBugsTest, "LetsDeepRock.Swarm.Gameplay.RecoveryBarrierFreezesBugs")
bool FSwarmGameplayRecoveryBarrierFreezesBugsTest::RunTest(const FString& Parameters)
{
	constexpr int32 BugCount = 10;
	constexpr float SpawnDistance = 900.f;
	constexpr float ChaseBeforeBarrierSeconds = 0.3f;
	constexpr float BarrierSettleSeconds = 0.1f;
	constexpr float PhaseSeconds = 2.f;
	constexpr double FrozenTolerance = 1.0;
	constexpr double MinResumeMove = 50.0;

	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, SpawnDistance + 200.f);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		SwarmGameplaySpawnAhead(Ctx, SwarmGameplayTestConstants::GruntRow, SpawnDistance, BugCount);
	});
	SwarmGameplayAddWorldWait(Context, ChaseBeforeBarrierSeconds);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		const AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		if (!GameMode || !GameState)
		{
			Ctx.Fail(TEXT("[Swarm] test map is not running AWeaponGameMode/AWeaponGameState"));
			return;
		}
		Ctx.Test->TestTrue(TEXT("EnterGameplayRecoveryBarrier succeeded"), GameMode->EnterGameplayRecoveryBarrier());
		Ctx.Test->TestTrue(TEXT("game state reports recovery blocked"), AWeaponGameState::IsGameplayRecoveryBlockedFor(Ctx.World.Get()));
	});
	SwarmGameplayAddWorldWait(Context, BarrierSettleSeconds);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		Ctx.Positions.Reset();
		for (const TWeakObjectPtr<ASwarmBugPawn>& Bug : Ctx.Bugs)
		{
			Ctx.Positions.Add(Bug.IsValid() ? Bug->GetActorLocation() : FVector::ZeroVector);
		}
		Ctx.ValueA = 0.0;
	});
	SwarmGameplayAddWorldWait(Context, PhaseSeconds, [](FSwarmGameplayTestContext& Ctx, float)
	{
		for (int32 Index = 0; Index < Ctx.Bugs.Num(); ++Index)
		{
			if (const ASwarmBugPawn* Bug = Ctx.Bugs[Index].Get())
			{
				Ctx.ValueA = FMath::Max(Ctx.ValueA, FVector::Distance(Bug->GetActorLocation(), Ctx.Positions[Index]));
			}
		}
		return true;
	});
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		SwarmGameplayCheck(Ctx, FString::Printf(TEXT("bugs frozen while blocked (max drift %.2f uu)"), Ctx.ValueA), Ctx.ValueA <= FrozenTolerance);
		Ctx.Test->TestEqual(TEXT("no bug lost while blocked"), SwarmGameplayCountTrackedAlive(Ctx), BugCount);

		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		Ctx.Test->TestTrue(TEXT("ReleaseGameplayRecoveryBarrier succeeded"), GameMode->ReleaseGameplayRecoveryBarrier());
		Ctx.Test->TestFalse(TEXT("game state reports recovery open"), AWeaponGameState::IsGameplayRecoveryBlockedFor(Ctx.World.Get()));
		Ctx.Positions.Reset();
		for (const TWeakObjectPtr<ASwarmBugPawn>& Bug : Ctx.Bugs)
		{
			Ctx.Positions.Add(Bug.IsValid() ? Bug->GetActorLocation() : FVector::ZeroVector);
		}
	});
	SwarmGameplayAddWorldWait(Context, PhaseSeconds);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		double Sum = 0.0;
		int32 Num = 0;
		for (int32 Index = 0; Index < Ctx.Bugs.Num(); ++Index)
		{
			if (const ASwarmBugPawn* Bug = Ctx.Bugs[Index].Get(); Bug && Bug->IsBugActive())
			{
				Sum += SwarmGameplayDist2D(Bug->GetActorLocation(), Ctx.Positions[Index]);
				++Num;
			}
		}
		const double MeanMove = Num > 0 ? Sum / Num : 0.0;
		SwarmGameplayCheck(Ctx, FString::Printf(TEXT("bugs resume after release (mean move %.1f >= %.0f)"), MeanMove, MinResumeMove), MeanMove >= MinResumeMove);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// 精英（HeavyTank BP）感知目标不会是虫群
// ---------------------------------------------------------------------------
SWARM_GAMEPLAY_TEST(FSwarmGameplayEliteIgnoresSwarmTest, "LetsDeepRock.Swarm.Gameplay.EliteIgnoresSwarm")
bool FSwarmGameplayEliteIgnoresSwarmTest::RunTest(const FString& Parameters)
{
	constexpr int32 BugCount = 30;
	constexpr float EliteDistance = 1200.f;
	constexpr float SwarmDistance = 600.f;
	constexpr float WatchSeconds = 4.f;
	constexpr float EliteSpawnZOffset = 90.f;

	UClass* EliteClass = LoadClass<AEnemyCharacter>(nullptr, SwarmGameplayTestConstants::HeavyTankClassPath);
	if (!TestNotNull(TEXT("BP_Enemy_HeavyTank class"), EliteClass))
	{
		return false;
	}

	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, EliteDistance + 200.f);
	SwarmGameplayAddStep(Context, [EliteClass = TStrongObjectPtr<UClass>(EliteClass)](FSwarmGameplayTestContext& Ctx)
	{
		// 精英生成方式与 UWeaponWaveDirectorComponent::SpawnOneEnemy 一致
		const FVector PlayerLocation = Ctx.Player->GetActorLocation();
		FVector EliteLocation = PlayerLocation + Ctx.OpenDirection * EliteDistance;
		FHitResult GroundHit;
		if (Ctx.World->LineTraceSingleByObjectType(GroundHit, EliteLocation, EliteLocation - FVector(0.0, 0.0, SwarmGameplayTestConstants::GroundProbeDepth),
			FCollisionObjectQueryParams(ECC_WorldStatic)))
		{
			EliteLocation.Z = GroundHit.ImpactPoint.Z + EliteSpawnZOffset;
		}
		const FTransform EliteTransform((-Ctx.OpenDirection).Rotation(), EliteLocation);
		AEnemyCharacter* Elite = Ctx.World->SpawnActorDeferred<AEnemyCharacter>(EliteClass.Get(), EliteTransform,
			nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Elite)
		{
			Ctx.Fail(TEXT("[Swarm] HeavyTank spawn failed"));
			return;
		}
		Elite->SetEnemyConfigRowName(SwarmGameplayTestConstants::HeavyTankRow);
		Elite->FinishSpawning(EliteTransform);
		Ctx.SpawnedActors.Add(Elite);

		SwarmGameplaySpawnAhead(Ctx, SwarmGameplayTestConstants::GruntRow, SwarmDistance, BugCount);
	});
	SwarmGameplayAddWorldWait(Context, WatchSeconds, [](FSwarmGameplayTestContext& Ctx, float)
	{
		const AEnemyCharacter* Elite = Ctx.SpawnedActors.Num() > 0 ? Cast<AEnemyCharacter>(Ctx.SpawnedActors[0].Get()) : nullptr;
		if (!Elite)
		{
			Ctx.Fail(TEXT("[Swarm] HeavyTank destroyed during watch"));
			return false;
		}
		const AEnemyAIController* Controller = Cast<AEnemyAIController>(Elite->GetController());
		if (!Controller)
		{
			return true;
		}
		Ctx.bFlagA = true;
		const AActor* Target = Controller->GetPerceivedTarget();
		if (Cast<ASwarmBugPawn>(Target))
		{
			++Ctx.CountA;
			Ctx.Note = GetNameSafe(Target);
		}
		Ctx.bFlagB |= Target && Target == Ctx.Player.Get();
		Ctx.bFlagC |= Target != nullptr;
		return true;
	});
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		Ctx.Test->TestTrue(TEXT("HeavyTank possessed by AEnemyAIController"), Ctx.bFlagA);
		// 感知必须真的工作过，否则“从不锁定虫群”无意义
		Ctx.Test->TestTrue(TEXT("HeavyTank perceived a target on at least one frame"), Ctx.bFlagC);
		Ctx.Test->TestEqual(FString::Printf(TEXT("frames perceiving a swarm pawn (last %s)"), *Ctx.Note), Ctx.CountA, 0);
		Ctx.Test->AddInfo(FString::Printf(TEXT("[Swarm] elite perceived player during watch: %s"), Ctx.bFlagB ? TEXT("yes") : TEXT("no")));
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// 主菜单世界不创建后端
// ---------------------------------------------------------------------------
SWARM_GAMEPLAY_TEST(FSwarmGameplayMenuWorldHasNoBackendTest, "LetsDeepRock.Swarm.Gameplay.MenuWorldHasNoBackend")
bool FSwarmGameplayMenuWorldHasNoBackendTest::RunTest(const FString& Parameters)
{
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	AddExpectedError(SwarmGameplayTestConstants::IgnoredFocusError, EAutomationExpectedErrorFlags::Contains, -1, false);
	SwarmGameplayAddOpenMap(Context, SwarmGameplayTestConstants::MenuMapPath);

	TSharedRef<double> StartRealTime = MakeShared<double>(-1.0);
	SwarmGameplayEnqueue([Context, StartRealTime]()
	{
		if (Context->bAborted)
		{
			SwarmGameplayRemoveMapDelegate(*Context);
			return true;
		}
		const double RealNow = FPlatformTime::Seconds();
		if (*StartRealTime < 0.0)
		{
			*StartRealTime = RealNow;
		}
		UWorld* World = Context->LoadedWorld.Get();
		if (World && World->GetMapName().Contains(SwarmGameplayTestConstants::MenuMapName) && World->HasBegunPlay())
		{
			SwarmGameplayRemoveMapDelegate(*Context);
			const USwarmSubsystem* Swarm = World->GetSubsystem<USwarmSubsystem>();
			Context->Test->TestNotNull(TEXT("menu game world still has the swarm subsystem"), Swarm);
			Context->Test->TestNull(TEXT("menu world has no swarm backend"), Swarm ? Swarm->GetBackend() : nullptr);
			return true;
		}
		if (RealNow - *StartRealTime > SwarmGameplayTestConstants::LoadTimeoutSeconds)
		{
			SwarmGameplayRemoveMapDelegate(*Context);
			Context->Fail(TEXT("[Swarm] main menu map did not load"));
			return true;
		}
		return false;
	});
	return true;
}

#undef SWARM_GAMEPLAY_TEST

#endif // WITH_DEV_AUTOMATION_TESTS
