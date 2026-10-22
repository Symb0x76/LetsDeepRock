// Swarm 池化虫群：关卡内玩法自动化测试（须以 -game 运行；打开 L_GridMapStage4Test 并驱动真实玩家/武器/手雷/GameMode 路径）

#include "Misc/AutomationTest.h"

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

namespace SwarmGameplayTestConstants
{
	inline const TCHAR* const TestMapPath = TEXT("/Game/GridMap/Maps/L_GridMapStage4Test");
	inline const TCHAR* const TestMapName = TEXT("L_GridMapStage4Test");
	inline const TCHAR* const MenuMapPath = TEXT("/Game/MainMenu/MainMenu");
	inline const TCHAR* const MenuMapName = TEXT("MainMenu");
	inline const TCHAR* const GrenadeDataPath = TEXT("/Game/Variant_Weapon/Weapons/DA_Grenade_Default.DA_Grenade_Default");
	inline const TCHAR* const HeavyTankClassPath = TEXT("/Game/Variant_Weapon/Enemies/BP_Enemy_HeavyTank.BP_Enemy_HeavyTank_C");
	inline const FName GruntRow(TEXT("BasicGrunt"));
	inline const FName ExploderRow(TEXT("Exploder"));
	inline const FName HeavyTankRow(TEXT("HeavyTank"));

	/** 加载地图/等待玩家就绪的真实时间上限 */
	inline constexpr double LoadTimeoutSeconds = 90.0;
	/** 世界时间等待的真实时间保险（世界时间停滞时避免挂死） */
	inline constexpr double WorldWaitRealTimeSlack = 30.0;
	/** 玩家就绪后的稳定等待（世界时间） */
	inline constexpr float SettleSeconds = 1.0f;
	/** “无敌”：抬高 Health/MaxHealth 基础值，虫群攻击不会致死 */
	inline constexpr float InvulnerableHealth = 1000000.f;
	/** 开阔方向搜索：采样方向数、扫掠球半径、扫掠抬高 */
	inline constexpr int32 OpenDirectionSamples = 16;
	inline constexpr float OpenProbeRadius = 60.f;
	inline constexpr float OpenProbeZOffset = 20.f;
	inline constexpr float GroundProbeDepth = 1000.f;
	/** 候选站位地面须与出生点地面同高（±该值），排除墙顶/岩石顶 */
	inline constexpr float ArenaGroundTolerance = 150.f;
	/** 传送时离地抬高，避免胶囊嵌入地面 */
	inline constexpr float ArenaDropHeight = 5.f;
	/** 测试图加载时 UI 打印的既有 Slate 错误（InputMode UIOnly 聚焦不可聚焦控件），与虫群无关，忽略 */
	inline const TCHAR* const IgnoredFocusError = TEXT("Attempting to focus Non-Focusable widget");
}

/** 单个用例的共享状态（各步骤 lambda 共享） */
struct FSwarmGameplayTestContext
{
	FAutomationTestBase* Test = nullptr;
	TWeakObjectPtr<UWorld> World;
	TWeakObjectPtr<AWeaponCharacter> Player;
	TWeakObjectPtr<AWeaponPlayerState> PlayerState;
	FDelegateHandle MapLoadedHandle;
	TWeakObjectPtr<UWorld> LoadedWorld;
	bool bAborted = false;

	/** 玩家周围最开阔的水平方向（单位向量） */
	FVector OpenDirection = FVector::ForwardVector;

	// 用例数据
	TArray<TWeakObjectPtr<ASwarmBugPawn>> Bugs;
	TArray<FVector> Positions;
	TArray<TWeakObjectPtr<AActor>> SpawnedActors;
	double ValueA = 0.0;
	double ValueB = 0.0;
	double ValueC = 0.0;
	int32 CountA = 0;
	bool bFlagA = false;
	bool bFlagB = false;
	bool bFlagC = false;
	TWeakObjectPtr<AActor> TrackedActor;
	FString Note;

	void Fail(const FString& Message)
	{
		Test->AddError(Message);
		bAborted = true;
	}

	ISwarmBackend* GetBackend() const
	{
		const USwarmSubsystem* Subsystem = World.IsValid() ? World->GetSubsystem<USwarmSubsystem>() : nullptr;
		return Subsystem ? Subsystem->GetBackend() : nullptr;
	}
};

using FSwarmGameplayTestContextRef = TSharedRef<FSwarmGameplayTestContext>;

static void SwarmGameplayEnqueue(TFunction<bool()> Predicate)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FFunctionLatentCommand>(MoveTemp(Predicate)));
}

static UWorld* SwarmGameplayFindGameWorld()
{
	if (!GEngine)
	{
		return nullptr;
	}
	for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
	{
		UWorld* World = WorldContext.World();
		if (World && (WorldContext.WorldType == EWorldType::Game || WorldContext.WorldType == EWorldType::PIE))
		{
			return World;
		}
	}
	return nullptr;
}

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

/** 单次步骤：前序步骤失败则跳过 */
static void SwarmGameplayAddStep(const FSwarmGameplayTestContextRef& Context, TFunction<void(FSwarmGameplayTestContext&)> Step)
{
	SwarmGameplayEnqueue([Context, Step = MoveTemp(Step)]()
	{
		if (Context->bAborted)
		{
			return true;
		}
		if (!Context->World.IsValid() || !Context->Player.IsValid())
		{
			Context->Fail(TEXT("[Swarm] test world or player lost"));
			return true;
		}
		Step(*Context);
		return true;
	});
}

/** 按世界时间等待 Seconds；PerFrame（可空）每帧调用，返回 false 表示提前结束等待 */
static void SwarmGameplayAddWorldWait(const FSwarmGameplayTestContextRef& Context, const float Seconds,
	TFunction<bool(FSwarmGameplayTestContext&, float)> PerFrame = nullptr)
{
	TSharedRef<double> StartWorldTime = MakeShared<double>(-1.0);
	TSharedRef<double> StartRealTime = MakeShared<double>(-1.0);
	SwarmGameplayEnqueue([Context, Seconds, PerFrame = MoveTemp(PerFrame), StartWorldTime, StartRealTime]()
	{
		if (Context->bAborted)
		{
			return true;
		}
		const UWorld* World = Context->World.Get();
		if (!World || !Context->Player.IsValid())
		{
			Context->Fail(TEXT("[Swarm] test world or player lost while waiting"));
			return true;
		}
		const double Now = World->GetTimeSeconds();
		const double RealNow = FPlatformTime::Seconds();
		if (*StartWorldTime < 0.0)
		{
			*StartWorldTime = Now;
			*StartRealTime = RealNow;
		}
		const float Elapsed = static_cast<float>(Now - *StartWorldTime);
		if (PerFrame && !PerFrame(*Context, Elapsed))
		{
			return true;
		}
		if (Context->bAborted)
		{
			return true;
		}
		if (RealNow - *StartRealTime > Seconds + SwarmGameplayTestConstants::WorldWaitRealTimeSlack)
		{
			Context->Fail(FString::Printf(TEXT("[Swarm] world time stalled: %.2f/%.2f s elapsed"), Elapsed, Seconds));
			return true;
		}
		return Elapsed >= Seconds;
	});
}

/** 打开地图并等待新世界加载完成（经 PostLoadMapWithWorld 区分新旧世界，同名地图强制重载也可靠） */
static void SwarmGameplayAddOpenMap(const FSwarmGameplayTestContextRef& Context, const TCHAR* MapPath)
{
	const FString Path(MapPath);
	SwarmGameplayEnqueue([Context, Path]()
	{
		Context->LoadedWorld.Reset();
		Context->MapLoadedHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddLambda([Context](UWorld* LoadedWorld)
		{
			Context->LoadedWorld = LoadedWorld;
		});
		UWorld* CurrentWorld = SwarmGameplayFindGameWorld();
		if (!GEngine || !CurrentWorld)
		{
			Context->Fail(TEXT("[Swarm] no game world to open map from (run with -game)"));
			return true;
		}
		GEngine->Exec(CurrentWorld, *FString::Printf(TEXT("open %s"), *Path));
		return true;
	});
}

static void SwarmGameplayRemoveMapDelegate(FSwarmGameplayTestContext& Context)
{
	if (Context.MapLoadedHandle.IsValid())
	{
		FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(Context.MapLoadedHandle);
		Context.MapLoadedHandle.Reset();
	}
}

/** 从 Origin 测量最开阔的水平方向：返回 RequiredDistance 内的最大无阻挡距离（终点下方须有地面） */
static double SwarmGameplayMeasureOpenDirection(UWorld& World, const FVector& Origin, const AActor* IgnoredActor,
	const float RequiredDistance, FVector& OutDirection)
{
	FCollisionObjectQueryParams Objects;
	Objects.AddObjectTypesToQuery(ECC_WorldStatic);
	Objects.AddObjectTypesToQuery(ECC_WorldDynamic);
	FCollisionQueryParams Params(SCENE_QUERY_STAT(SwarmGameplayOpenDirection), false, IgnoredActor);
	const FCollisionShape Probe = FCollisionShape::MakeSphere(SwarmGameplayTestConstants::OpenProbeRadius);
	const FVector ProbeOrigin = Origin + FVector(0.0, 0.0, SwarmGameplayTestConstants::OpenProbeZOffset);

	double BestDistance = -1.0;
	OutDirection = FVector::ForwardVector;
	for (int32 Index = 0; Index < SwarmGameplayTestConstants::OpenDirectionSamples; ++Index)
	{
		const double Yaw = 360.0 * Index / SwarmGameplayTestConstants::OpenDirectionSamples;
		const FVector Direction = FRotator(0.0, Yaw, 0.0).Vector();
		FHitResult Hit;
		const bool bBlocked = World.SweepSingleByObjectType(Hit, ProbeOrigin, ProbeOrigin + Direction * RequiredDistance,
			FQuat::Identity, Objects, Probe, Params);
		double Clear = bBlocked ? Hit.Distance : RequiredDistance;

		// 终点下方必须有地面，否则虫会被生成到别处
		if (!bBlocked)
		{
			FHitResult GroundHit;
			const FVector End = ProbeOrigin + Direction * RequiredDistance;
			if (!World.LineTraceSingleByObjectType(GroundHit, End, End - FVector(0.0, 0.0, SwarmGameplayTestConstants::GroundProbeDepth),
				FCollisionObjectQueryParams(ECC_WorldStatic), Params))
			{
				Clear = 0.0;
			}
		}
		if (Clear > BestDistance)
		{
			BestDistance = Clear;
			OutDirection = Direction;
		}
	}
	return BestDistance;
}

/**
 * 为用例选定站位：先试玩家出生点，不够开阔则依次试各房间中心（仅接受与出生点同一地面高度的点）。
 * 选中房间时把玩家传送过去；返回 false 表示整张图都没有足够空地。
 */
static bool SwarmGameplayChooseArena(FSwarmGameplayTestContext& Context, const float RequiredDistance)
{
	UWorld* World = Context.World.Get();
	AWeaponCharacter* Player = Context.Player.Get();
	const double HalfHeight = Player->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
	const FVector Spawn = Player->GetActorLocation();
	const double SpawnGroundZ = Spawn.Z - HalfHeight;

	TArray<FVector> Candidates;
	Candidates.Add(Spawn);
	if (const AGridMapManager* Grid = FGridDamageHelper::FindGridMapManager(Player))
	{
		const EGridMapRoomPurpose Purposes[] = {EGridMapRoomPurpose::Combat, EGridMapRoomPurpose::Objective,
			EGridMapRoomPurpose::Branch, EGridMapRoomPurpose::Spawn, EGridMapRoomPurpose::Connector};
		FCollisionQueryParams Params(SCENE_QUERY_STAT(SwarmGameplayArenaGround), false, Player);
		for (const EGridMapRoomPurpose Purpose : Purposes)
		{
			TArray<FBox2D> Rooms;
			Grid->GetRoomWorldBoundsByPurpose(Purpose, Rooms);
			for (const FBox2D& Room : Rooms)
			{
				const FVector2D Center = Room.GetCenter();
				const FVector Top(Center, SpawnGroundZ + SwarmGameplayTestConstants::ArenaGroundTolerance);
				FHitResult GroundHit;
				if (World->LineTraceSingleByObjectType(GroundHit, Top, Top - FVector(0.0, 0.0, 2.0 * SwarmGameplayTestConstants::ArenaGroundTolerance),
					FCollisionObjectQueryParams(ECC_WorldStatic), Params))
				{
					Candidates.Add(FVector(Center, GroundHit.ImpactPoint.Z + HalfHeight + SwarmGameplayTestConstants::ArenaDropHeight));
				}
			}
		}
	}

	double Best = -1.0;
	for (const FVector& Candidate : Candidates)
	{
		FVector Direction;
		const double Clear = SwarmGameplayMeasureOpenDirection(*World, Candidate, Player, RequiredDistance, Direction);
		Best = FMath::Max(Best, Clear);
		if (Clear >= RequiredDistance)
		{
			if (!Candidate.Equals(Spawn))
			{
				Player->TeleportTo(Candidate, Player->GetActorRotation());
				Context.Test->AddInfo(FString::Printf(TEXT("[Swarm] moved player to arena %s"), *Candidate.ToCompactString()));
			}
			return true;
		}
	}
	Context.Fail(FString::Printf(TEXT("[Swarm] no arena with %.0f uu of open ground among %d candidates (best %.0f uu)"),
		RequiredDistance, Candidates.Num(), Best));
	return false;
}

/** 在玩家当前站位确定开阔方向 */
static bool SwarmGameplayFindOpenDirection(FSwarmGameplayTestContext& Context, const float RequiredDistance)
{
	const double Clear = SwarmGameplayMeasureOpenDirection(*Context.World, Context.Player->GetActorLocation(), Context.Player.Get(),
		RequiredDistance, Context.OpenDirection);
	if (Clear < RequiredDistance)
	{
		Context.Fail(FString::Printf(TEXT("[Swarm] no open direction of %.0f uu around player (best %.0f uu)"), RequiredDistance, Clear));
		return false;
	}
	Context.Test->AddInfo(FString::Printf(TEXT("[Swarm] open direction yaw %.1f"), Context.OpenDirection.Rotation().Yaw));
	return true;
}

/** 共享准备：打开测试图、等玩家落地、无敌、停波次导演、清场 */
static void SwarmGameplayAddArrange(const FSwarmGameplayTestContextRef& Context, const float RequiredOpenDistance)
{
	Context->Test->AddExpectedError(SwarmGameplayTestConstants::IgnoredFocusError, EAutomationExpectedErrorFlags::Contains, -1, false);
	SwarmGameplayAddOpenMap(Context, SwarmGameplayTestConstants::TestMapPath);

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
		FString Missing = TEXT("map load");
		if (World && World->GetMapName().Contains(SwarmGameplayTestConstants::TestMapName) && World->HasBegunPlay())
		{
			APlayerController* Controller = World->GetFirstPlayerController();
			AWeaponCharacter* Character = Controller ? Cast<AWeaponCharacter>(Controller->GetPawn()) : nullptr;
			AWeaponPlayerState* PlayerState = Controller ? Controller->GetPlayerState<AWeaponPlayerState>() : nullptr;
			const USwarmSubsystem* Swarm = World->GetSubsystem<USwarmSubsystem>();
			Missing = !Character ? TEXT("player pawn")
				: !PlayerState || !PlayerState->GetAbilitySystemComponent() || !PlayerState->GetHealthSet() ? TEXT("player state / ASC")
				: !Swarm || !Swarm->GetBackend() ? TEXT("swarm backend")
				: !Character->GetCharacterMovement() || !Character->GetCharacterMovement()->IsMovingOnGround() ? TEXT("player on ground")
				: TEXT("");
			if (Missing.IsEmpty())
			{
				Context->World = World;
				Context->Player = Character;
				Context->PlayerState = PlayerState;
				SwarmGameplayRemoveMapDelegate(*Context);
				return true;
			}
		}
		if (RealNow - *StartRealTime > SwarmGameplayTestConstants::LoadTimeoutSeconds)
		{
			SwarmGameplayRemoveMapDelegate(*Context);
			Context->Fail(FString::Printf(TEXT("[Swarm] test map not ready after %.0f s, waiting on: %s"),
				SwarmGameplayTestConstants::LoadTimeoutSeconds, *Missing));
			return true;
		}
		return false;
	});

	SwarmGameplayAddWorldWait(Context, SwarmGameplayTestConstants::SettleSeconds);

	SwarmGameplayAddStep(Context, [RequiredOpenDistance](FSwarmGameplayTestContext& Ctx)
	{
		UWorld* World = Ctx.World.Get();

		// 无敌：先抬 MaxHealth 再抬 Health（Health 被钳制到 MaxHealth）
		UAbilitySystemComponent* ASC = Ctx.PlayerState->GetAbilitySystemComponent();
		ASC->SetNumericAttributeBase(UWeaponHealthSet::GetMaxHealthAttribute(), SwarmGameplayTestConstants::InvulnerableHealth);
		ASC->SetNumericAttributeBase(UWeaponHealthSet::GetHealthAttribute(), SwarmGameplayTestConstants::InvulnerableHealth);

		// 停波次导演，避免房间触发的虫群/精英干扰用例
		if (AGameModeBase* GameMode = World->GetAuthGameMode())
		{
			if (UWeaponWaveDirectorComponent* Director = GameMode->FindComponentByClass<UWeaponWaveDirectorComponent>())
			{
				Director->SetComponentTickEnabled(false);
			}
		}
		for (TActorIterator<AEnemyCharacter> It(World); It; ++It)
		{
			It->Destroy();
		}

		ISwarmBackend* Backend = Ctx.GetBackend();
		Backend->DespawnAll();
		if (Backend->CountAlive() != 0)
		{
			Ctx.Fail(FString::Printf(TEXT("[Swarm] DespawnAll left %d alive"), Backend->CountAlive()));
			return;
		}
		SwarmGameplayChooseArena(Ctx, RequiredOpenDistance);
	});
	SwarmGameplayAddWorldWait(Context, SwarmGameplayTestConstants::SettleSeconds);
	SwarmGameplayAddStep(Context, [RequiredOpenDistance](FSwarmGameplayTestContext& Ctx)
	{
		if (!Ctx.Player->GetCharacterMovement()->IsMovingOnGround())
		{
			Ctx.Fail(TEXT("[Swarm] player not on ground after arena move"));
			return;
		}
		SwarmGameplayFindOpenDirection(Ctx, RequiredOpenDistance);
	});
}

/** 收尾：无论成败都释放恢复屏障、销毁生成的 Actor、清空虫群 */
static void SwarmGameplayAddCleanup(const FSwarmGameplayTestContextRef& Context)
{
	SwarmGameplayEnqueue([Context]()
	{
		SwarmGameplayRemoveMapDelegate(*Context);
		if (UWorld* World = Context->World.Get())
		{
			if (AWeaponGameMode* GameMode = World->GetAuthGameMode<AWeaponGameMode>())
			{
				GameMode->ReleaseGameplayRecoveryBarrier();
			}
			for (const TWeakObjectPtr<AActor>& Actor : Context->SpawnedActors)
			{
				if (Actor.IsValid())
				{
					Actor->Destroy();
				}
			}
			if (ISwarmBackend* Backend = Context->GetBackend())
			{
				Backend->DespawnAll();
			}
		}
		return true;
	});
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

static FSwarmGameplayTestContextRef SwarmGameplayMakeContext(FAutomationTestBase* Test)
{
	FSwarmGameplayTestContextRef Context = MakeShared<FSwarmGameplayTestContext>();
	Context->Test = Test;
	return Context;
}

/** 断言并把实测值写入测试日志（通过时也可见，便于观察余量与抖动） */
static void SwarmGameplayCheck(FSwarmGameplayTestContext& Context, const FString& What, const bool bValue)
{
	Context.Test->AddInfo(FString::Printf(TEXT("[Swarm] %s: %s"), bValue ? TEXT("ok") : TEXT("FAIL"), *What));
	Context.Test->TestTrue(What, bValue);
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
