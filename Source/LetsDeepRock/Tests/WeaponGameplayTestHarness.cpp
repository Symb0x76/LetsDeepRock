// 关卡内玩法自动化测试的共享测试框架实现（见 WeaponGameplayTestHarness.h）

#include "WeaponGameplayTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AbilitySystemComponent.h"
#include "Components/CapsuleComponent.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "AI/WeaponWaveDirectorComponent.h"
#include "Attributes/WeaponHealthSet.h"
#include "EnemyCharacter.h"
#include "GridMap/GridDamageHelper.h"
#include "GridMap/GridMapManager.h"
#include "WeaponGameMode.h"

void SwarmGameplayEnqueue(TFunction<bool()> Predicate)
{
	FAutomationTestFramework::Get().EnqueueLatentCommand(MakeShared<FFunctionLatentCommand>(MoveTemp(Predicate)));
}

UWorld* SwarmGameplayFindGameWorld()
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

/** 单次步骤：前序步骤失败则跳过 */
void SwarmGameplayAddStep(const FSwarmGameplayTestContextRef& Context, TFunction<void(FSwarmGameplayTestContext&)> Step)
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
void SwarmGameplayAddWorldWait(const FSwarmGameplayTestContextRef& Context, const float Seconds,
	TFunction<bool(FSwarmGameplayTestContext&, float)> PerFrame)
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
void SwarmGameplayAddOpenMap(const FSwarmGameplayTestContextRef& Context, const TCHAR* MapPath)
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

void SwarmGameplayRemoveMapDelegate(FSwarmGameplayTestContext& Context)
{
	if (Context.MapLoadedHandle.IsValid())
	{
		FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(Context.MapLoadedHandle);
		Context.MapLoadedHandle.Reset();
	}
}

/** 从 Origin 测量最开阔的水平方向：返回 RequiredDistance 内的最大无阻挡距离（终点下方须有地面） */
double SwarmGameplayMeasureOpenDirection(UWorld& World, const FVector& Origin, const AActor* IgnoredActor,
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
bool SwarmGameplayChooseArena(FSwarmGameplayTestContext& Context, const float RequiredDistance)
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
bool SwarmGameplayFindOpenDirection(FSwarmGameplayTestContext& Context, const float RequiredDistance)
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
void SwarmGameplayAddArrange(const FSwarmGameplayTestContextRef& Context, const float RequiredOpenDistance)
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
void SwarmGameplayAddCleanup(const FSwarmGameplayTestContextRef& Context)
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

FSwarmGameplayTestContextRef SwarmGameplayMakeContext(FAutomationTestBase* Test)
{
	FSwarmGameplayTestContextRef Context = MakeShared<FSwarmGameplayTestContext>();
	Context->Test = Test;
	return Context;
}

/** 断言并把实测值写入测试日志（通过时也可见，便于观察余量与抖动） */
void SwarmGameplayCheck(FSwarmGameplayTestContext& Context, const FString& What, const bool bValue)
{
	Context.Test->AddInfo(FString::Printf(TEXT("[Swarm] %s: %s"), bValue ? TEXT("ok") : TEXT("FAIL"), *What));
	Context.Test->TestTrue(What, bValue);
}

#endif // WITH_DEV_AUTOMATION_TESTS
