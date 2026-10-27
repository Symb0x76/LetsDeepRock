// 增援池 / 生存状态 / 任务失败:关卡内玩法自动化测试(须以 -game 运行;复用 WeaponGameplayTestHarness)

#include "Misc/AutomationTest.h"
#include "WeaponGameplayTestHarness.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "AbilitySystemComponent.h"
#include "AI/WeaponWaveDirectorComponent.h"
#include "Attributes/WeaponHealthSet.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "EnemyCharacter.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameplayEffect.h"
#include "GridMap/GridMapObjectiveActors.h"
#include "Net/OnlineEngineInterface.h"
#include "Online/WeaponSnapshot.h"
#include "Online/WeaponSnapshotCoordinator.h"
#include "Reinforcement/WeaponReinforcementRules.h"
#include "UObject/Package.h"
#include "WeaponCharacter.h"
#include "WeaponGameplayTags.h"
#include "WeaponGameMode.h"
#include "WeaponGameState.h"
#include "WeaponPlayerState.h"

#define REINFORCE_GAMEPLAY_TEST(ClassName, Path) IMPLEMENT_SIMPLE_AUTOMATION_TEST(ClassName, Path, EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

namespace ReinforceGameplayTestConstants
{
	/** Starting 倒计时(默认 3 s)+ 余量的世界时间上限 */
	inline constexpr float StartMatchMaxSeconds = 15.f;
	/** 测试用自动增援延迟 */
	inline constexpr float ShortReinforceDelay = 0.5f;
	/** 一击必杀的伤害量(Health 基础值被压到 1) */
	inline constexpr float LethalDamage = 10.f;
	/** 结算 5 s(EndingDuration)+ 重生延迟 3 s(RespawnDelay)+ 余量 */
	inline constexpr float EndingPlusRespawnSeconds = 9.5f;
}

/** 按世界时间等待;每帧先把 Player 刷新为控制器当前 Pawn(增援会销毁旧 Pawn),PerFrame 返回 false 提前结束 */
static void ReinforceAddWait(const FSwarmGameplayTestContextRef& Context, const float Seconds,
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
		if (!World)
		{
			Context->Fail(TEXT("[Reinforce] test world lost while waiting"));
			return true;
		}
		if (const APlayerController* Controller = World->GetFirstPlayerController())
		{
			if (AWeaponCharacter* Pawn = Cast<AWeaponCharacter>(Controller->GetPawn()))
			{
				Context->Player = Pawn;
			}
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
		if (RealNow - *StartRealTime > Seconds + SwarmGameplayTestConstants::WorldWaitRealTimeSlack)
		{
			Context->Fail(FString::Printf(TEXT("[Reinforce] world time stalled: %.2f/%.2f s elapsed"), Elapsed, Seconds));
			return true;
		}
		return Elapsed >= Seconds;
	});
}

/** Arrange 的清场:停波次导演、清敌人与虫群(开局会重新启动刷怪,需再清一次) */
static void ReinforceClearHostiles(FSwarmGameplayTestContext& Ctx)
{
	UWorld* World = Ctx.World.Get();
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
	if (ISwarmBackend* Backend = Ctx.GetBackend())
	{
		Backend->DespawnAll();
	}
}

/**
 * 在 Arrange 之后开一局真实对局(单人 -game):把本地 PlayerState 置 ready,调用 TryStartWeaponMatch,
 * 等 Starting 倒计时走完进入 InGame,再重新清场。
 */
static void ReinforceAddStartMatch(const FSwarmGameplayTestContextRef& Context)
{
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		const AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		if (!GameMode || !GameState)
		{
			Ctx.Fail(TEXT("[Reinforce] test map is not running AWeaponGameMode/AWeaponGameState"));
			return;
		}
		Ctx.PlayerState->SetPlayerReady(true);
		if (GameState->GetMatchPhase() == EWeaponMatchPhase::Waiting && !GameMode->TryStartWeaponMatch())
		{
			Ctx.Fail(TEXT("[Reinforce] TryStartWeaponMatch refused with the local player ready"));
		}
	});
	ReinforceAddWait(Context, ReinforceGameplayTestConstants::StartMatchMaxSeconds,
		[](FSwarmGameplayTestContext& Ctx, float)
		{
			const AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
			return !GameState || GameState->GetMatchPhase() != EWeaponMatchPhase::InGame;
		});
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		const AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		if (!GameState || GameState->GetMatchPhase() != EWeaponMatchPhase::InGame)
		{
			Ctx.Fail(TEXT("[Reinforce] match did not reach InGame"));
			return;
		}
		ReinforceClearHostiles(Ctx);
	});
}

/** 对本地玩家施加一次瞬时 Damage 效果(每次新建匿名 Effect,同一测试里可重复调用) */
static void ReinforceApplyDamage(FSwarmGameplayTestContext& Ctx, const float Damage)
{
	UAbilitySystemComponent* ASC = Ctx.PlayerState->GetAbilitySystemComponent();
	UGameplayEffect* Effect = NewObject<UGameplayEffect>(GetTransientPackage());
	Effect->DurationPolicy = EGameplayEffectDurationType::Instant;
	FGameplayModifierInfo& Modifier = Effect->Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UWeaponHealthSet::GetDamageAttribute();
	Modifier.ModifierOp = EGameplayModOp::Additive;
	Modifier.ModifierMagnitude = FScalableFloat(Damage);
	ASC->ApplyGameplayEffectToSelf(Effect, 1.f, ASC->MakeEffectContext());
}

/** 一击必杀:护盾清零、Health 基础值压到 1,再施加一次瞬时 Damage 效果 */
static void ReinforceKill(FSwarmGameplayTestContext& Ctx)
{
	UAbilitySystemComponent* ASC = Ctx.PlayerState->GetAbilitySystemComponent();
	ASC->SetNumericAttributeBase(UWeaponHealthSet::GetMaxShieldAttribute(), 0.f);
	ASC->SetNumericAttributeBase(UWeaponHealthSet::GetShieldAttribute(), 0.f);
	ASC->SetNumericAttributeBase(UWeaponHealthSet::GetHealthAttribute(), 1.f);
	ReinforceApplyDamage(Ctx, ReinforceGameplayTestConstants::LethalDamage);
}

/** 当前服务器世界时间(与 PlayerState 上的绝对时间戳同一时基) */
static float ReinforceServerNow(const FSwarmGameplayTestContext& Ctx)
{
	return Ctx.World->GetGameState<AWeaponGameState>()->GetServerWorldTimeSeconds();
}

/** 死亡日志([DeathTrace])以 Error 级输出,属预期 */
static void ReinforceExpectDeathTrace(FAutomationTestBase& Test)
{
	Test.AddExpectedError(TEXT("[DeathTrace]"), EAutomationExpectedErrorFlags::Contains, -1, false);
}

// ---------------------------------------------------------------------------
// 开局时增援池 = 难度对应值
// ---------------------------------------------------------------------------
REINFORCE_GAMEPLAY_TEST(FReinforceGameplayPoolFromDifficultyTest, "LetsDeepRock.Reinforcement.Gameplay.PoolFromDifficulty")
bool FReinforceGameplayPoolFromDifficultyTest::RunTest(const FString& Parameters)
{
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		const AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		const AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		const int32 Expected = WeaponReinforcementPoolForDifficulty(
			GameState->GetMissionSettings().Difficulty, GameMode->ReinforcementPoolByDifficulty);
		Ctx.Test->TestEqual(TEXT("pool equals the difficulty pool"), GameState->GetReinforcementPool(), Expected);
		Ctx.Test->TestTrue(TEXT("pool is positive"), GameState->GetReinforcementPool() > 0);
		Ctx.Test->TestEqual(TEXT("outcome is None"), GameState->GetMissionOutcome(), EWeaponMissionOutcome::None);
		Ctx.Test->TestEqual(TEXT("local player is Up"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Up);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// 死亡不扣池;30 s(测试里 0.5 s)自动增援真正复活时才扣 1
// ---------------------------------------------------------------------------
REINFORCE_GAMEPLAY_TEST(FReinforceGameplayAutoReinforceSpendsPoolTest, "LetsDeepRock.Reinforcement.Gameplay.AutoReinforceSpendsPool")
bool FReinforceGameplayAutoReinforceSpendsPoolTest::RunTest(const FString& Parameters)
{
	ReinforceExpectDeathTrace(*this);
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		GameMode->AutoReinforceDelay = ReinforceGameplayTestConstants::ShortReinforceDelay;
		GameState->SetReinforcementPoolAuthority(2);
		ReinforceKill(Ctx);

		Ctx.Test->TestEqual(TEXT("life state is Dead"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);
		Ctx.Test->TestTrue(TEXT("respawn timestamp published"), Ctx.PlayerState->GetRespawnServerTime() > 0.f);
		Ctx.Test->TestEqual(TEXT("dying does not spend the pool"), GameState->GetReinforcementPool(), 2);
		Ctx.Test->TestEqual(TEXT("mission continues"), GameState->GetMatchPhase(), EWeaponMatchPhase::InGame);
	});
	ReinforceAddWait(Context, 1.0f);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		const AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		const APlayerController* Controller = Ctx.World->GetFirstPlayerController();
		Ctx.Test->TestEqual(TEXT("life state is Up again"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Up);
		Ctx.Test->TestEqual(TEXT("reinforcing spent one life"), GameState->GetReinforcementPool(), 1);
		const APawn* NewPawn = Controller ? Controller->GetPawn() : nullptr;
		Ctx.Test->TestNotNull(TEXT("controller owns a pawn"), NewPawn);
		Ctx.Test->TestEqual(TEXT("respawn timestamp cleared"), Ctx.PlayerState->GetRespawnServerTime(), 0.f);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// 池空 + 全员倒下 → 任务失败
// ---------------------------------------------------------------------------
REINFORCE_GAMEPLAY_TEST(FReinforceGameplayEmptyPoolWipeFailsTest, "LetsDeepRock.Reinforcement.Gameplay.EmptyPoolWipeFails")
bool FReinforceGameplayEmptyPoolWipeFailsTest::RunTest(const FString& Parameters)
{
	ReinforceExpectDeathTrace(*this);
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		GameMode->AutoReinforceDelay = ReinforceGameplayTestConstants::ShortReinforceDelay;
		GameState->SetReinforcementPoolAuthority(0);
		ReinforceKill(Ctx);

		Ctx.Test->TestEqual(TEXT("life state is Dead"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);
		Ctx.Test->TestEqual(TEXT("phase is Ending"), GameState->GetMatchPhase(), EWeaponMatchPhase::Ending);
		Ctx.Test->TestEqual(TEXT("outcome is Failed"), GameState->GetMissionOutcome(), EWeaponMissionOutcome::Failed);
		// 池空不排增援;失败结算开始时才按 RespawnDelay 排免费复活(时间戳 > 0)
		Ctx.Test->TestTrue(TEXT("free respawn scheduled only once the match ended"), Ctx.PlayerState->GetRespawnServerTime() > 0.f);
	});
	// 结算(EndingDuration 5 s)中应以 RespawnDelay(3 s)免费复活死者,留足余量
	ReinforceAddWait(Context, ReinforceGameplayTestConstants::EndingPlusRespawnSeconds);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		const APlayerController* Controller = Ctx.World->GetFirstPlayerController();
		const AWeaponCharacter* NewPawn = Controller ? Cast<AWeaponCharacter>(Controller->GetPawn()) : nullptr;
		Ctx.Test->TestNotNull(TEXT("dead player got a pawn back after the match ended"), NewPawn);
		Ctx.Test->TestEqual(TEXT("life state is Up after the match ended"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Up);
		Ctx.Test->TestFalse(TEXT("State.Dead tag cleared"),
			Ctx.PlayerState->GetAbilitySystemComponent()->HasMatchingGameplayTag(TAG_State_Dead));
		Ctx.Test->TestEqual(TEXT("back to Waiting"), Ctx.World->GetGameState<AWeaponGameState>()->GetMatchPhase(), EWeaponMatchPhase::Waiting);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// 局内职业选择的"立即重生"不能绕开增援池
// ---------------------------------------------------------------------------
REINFORCE_GAMEPLAY_TEST(FReinforceGameplayProfessionRespawnBlockedWhenOutTest, "LetsDeepRock.Reinforcement.Gameplay.ProfessionRespawnBlockedWhenOut")
bool FReinforceGameplayProfessionRespawnBlockedWhenOutTest::RunTest(const FString& Parameters)
{
	ReinforceExpectDeathTrace(*this);
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		GameMode->AutoReinforceDelay = 30.f;
		GameState->SetReinforcementPoolAuthority(1);
		ReinforceKill(Ctx);
		Ctx.Test->TestEqual(TEXT("life state is Dead"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);

		AController* Controller = Ctx.World->GetFirstPlayerController();
		GameMode->RespawnPlayerImmediately(Controller);
		Ctx.Test->TestEqual(TEXT("still Dead after immediate respawn"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);
		Ctx.Test->TestEqual(TEXT("pool untouched"), GameState->GetReinforcementPool(), 1);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// 倒地 / 流血(solo 测试把 bDownedRequiresReviver 关掉;Arrange 的 MaxHealth 1e6 在受伤前压回 100)
// ---------------------------------------------------------------------------
REINFORCE_GAMEPLAY_TEST(FReinforceGameplayZeroHealthGoesDownedTest, "LetsDeepRock.Reinforcement.Gameplay.ZeroHealthGoesDowned")
bool FReinforceGameplayZeroHealthGoesDownedTest::RunTest(const FString& Parameters)
{
	ReinforceExpectDeathTrace(*this);
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		GameMode->bDownedRequiresReviver = false;
		GameMode->BleedOutSeconds = 2.f;
		ReinforceKill(Ctx);

		const UAbilitySystemComponent* ASC = Ctx.PlayerState->GetAbilitySystemComponent();
		Ctx.Test->TestEqual(TEXT("life state is Downed"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Downed);
		Ctx.Test->TestTrue(TEXT("bleed-out deadline is in the future"), Ctx.PlayerState->GetDownedServerTime() > ReinforceServerNow(Ctx));
		Ctx.Test->TestTrue(TEXT("State.Downed tag set"), ASC->HasMatchingGameplayTag(TAG_State_Downed));
		Ctx.Test->TestFalse(TEXT("State.Dead tag not set"), ASC->HasMatchingGameplayTag(TAG_State_Dead));
		const AWeaponCharacter* Pawn = Cast<AWeaponCharacter>(Ctx.World->GetFirstPlayerController()->GetPawn());
		Ctx.Test->TestNotNull(TEXT("downed player keeps its pawn"), Pawn);
		if (Pawn)
		{
			Ctx.Test->TestFalse(TEXT("pawn stays visible"), Pawn->IsHidden());
			Ctx.Test->TestEqual(TEXT("movement disabled"), Pawn->GetCharacterMovement()->MovementMode.GetValue(), static_cast<uint8>(MOVE_None));
		}
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

REINFORCE_GAMEPLAY_TEST(FReinforceGameplayBleedOutKillsTest, "LetsDeepRock.Reinforcement.Gameplay.BleedOutKills")
bool FReinforceGameplayBleedOutKillsTest::RunTest(const FString& Parameters)
{
	ReinforceExpectDeathTrace(*this);
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		GameMode->bDownedRequiresReviver = false;
		GameMode->BleedOutSeconds = 2.f;
		GameMode->AutoReinforceDelay = 30.f;
		ReinforceKill(Ctx);
		Ctx.Test->TestEqual(TEXT("life state is Downed"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Downed);
	});
	ReinforceAddWait(Context, 2.5f);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		const AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		Ctx.Test->TestTrue(TEXT("pool is positive"), GameState->GetReinforcementPool() > 0);
		Ctx.Test->TestEqual(TEXT("life state is Dead after bleed-out"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);
		Ctx.Test->TestEqual(TEXT("downed timestamp cleared"), Ctx.PlayerState->GetDownedServerTime(), 0.f);
		Ctx.Test->TestTrue(TEXT("auto-reinforce scheduled"), Ctx.PlayerState->GetRespawnServerTime() > 0.f);
		Ctx.Test->TestFalse(TEXT("State.Downed cleared"), Ctx.PlayerState->GetAbilitySystemComponent()->HasMatchingGameplayTag(TAG_State_Downed));
		Ctx.Test->TestTrue(TEXT("State.Dead set"), Ctx.PlayerState->GetAbilitySystemComponent()->HasMatchingGameplayTag(TAG_State_Dead));
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

REINFORCE_GAMEPLAY_TEST(FReinforceGameplaySoloSkipsDownedTest, "LetsDeepRock.Reinforcement.Gameplay.SoloSkipsDowned")
bool FReinforceGameplaySoloSkipsDownedTest::RunTest(const FString& Parameters)
{
	ReinforceExpectDeathTrace(*this);
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		GameMode->bDownedRequiresReviver = true;
		GameMode->AutoReinforceDelay = 30.f;
		ReinforceKill(Ctx);
		Ctx.Test->TestEqual(TEXT("no other player is up, so the lethal hit kills outright"),
			Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);
		Ctx.Test->TestEqual(TEXT("no downed timestamp"), Ctx.PlayerState->GetDownedServerTime(), 0.f);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

REINFORCE_GAMEPLAY_TEST(FReinforceGameplayDamageShortensBleedOutTest, "LetsDeepRock.Reinforcement.Gameplay.DamageShortensBleedOut")
bool FReinforceGameplayDamageShortensBleedOutTest::RunTest(const FString& Parameters)
{
	ReinforceExpectDeathTrace(*this);
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		GameMode->bDownedRequiresReviver = false;
		GameMode->BleedOutSeconds = 10.f;
		GameMode->AutoReinforceDelay = 30.f;
		GameState->SetReinforcementPoolAuthority(1);
		ReinforceKill(Ctx);
		Ctx.Test->TestEqual(TEXT("life state is Downed"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Downed);

		// 受伤前把 MaxHealth 压回 100(Arrange 抬到了 1e6):50 点伤害 = 5 s。
		Ctx.PlayerState->GetAbilitySystemComponent()->SetNumericAttributeBase(UWeaponHealthSet::GetMaxHealthAttribute(), 100.f);
		ReinforceApplyDamage(Ctx, 50.f);
		Ctx.Test->TestEqual(TEXT("still Downed after half the bar"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Downed);
		Ctx.Test->TestEqual(TEXT("bleed-out shortened to about 5 s"),
			Ctx.PlayerState->GetDownedServerTime() - ReinforceServerNow(Ctx), 5.f, 0.2f);

		// 再受 60 点(6 s > 剩余 5 s)→ 死亡,走一次 PlayerDied。
		ReinforceApplyDamage(Ctx, 60.f);
		Ctx.Test->TestEqual(TEXT("life state is Dead"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);
		Ctx.Test->TestEqual(TEXT("downed timestamp cleared"), Ctx.PlayerState->GetDownedServerTime(), 0.f);
		const float RespawnTime = Ctx.PlayerState->GetRespawnServerTime();
		Ctx.Test->TestTrue(TEXT("auto-reinforce scheduled"), RespawnTime > 0.f);
		Ctx.ValueA = RespawnTime;

		// 死者再受伤:不得再次走 PlayerDied(否则会重排计时,RespawnServerTime 会移动)。
		ReinforceApplyDamage(Ctx, 10.f);
		Ctx.Test->TestEqual(TEXT("still Dead"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);
		Ctx.Test->TestEqual(TEXT("respawn timestamp unchanged by a second hit"),
			static_cast<double>(Ctx.PlayerState->GetRespawnServerTime()), Ctx.ValueA);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

REINFORCE_GAMEPLAY_TEST(FReinforceGameplayBarrierPausesBleedOutTest, "LetsDeepRock.Reinforcement.Gameplay.BarrierPausesBleedOut")
bool FReinforceGameplayBarrierPausesBleedOutTest::RunTest(const FString& Parameters)
{
	ReinforceExpectDeathTrace(*this);
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		GameMode->bDownedRequiresReviver = false;
		GameMode->BleedOutSeconds = 2.f;
		GameMode->AutoReinforceDelay = 30.f;
		ReinforceKill(Ctx);
		Ctx.Test->TestEqual(TEXT("life state is Downed"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Downed);
		Ctx.Test->TestTrue(TEXT("barrier entered"), GameMode->EnterGameplayRecoveryBarrier());
	});
	ReinforceAddWait(Context, 3.f);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		Ctx.Test->TestEqual(TEXT("still Downed through the barrier"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Downed);
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		Ctx.Test->TestTrue(TEXT("barrier released"), GameMode->ReleaseGameplayRecoveryBarrier());
		Ctx.Test->TestTrue(TEXT("deadline re-anchored in the future"), Ctx.PlayerState->GetDownedServerTime() > ReinforceServerNow(Ctx));
	});
	ReinforceAddWait(Context, 2.5f);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		Ctx.Test->TestEqual(TEXT("bled out after the barrier opened"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

REINFORCE_GAMEPLAY_TEST(FReinforceGameplayLeaverClearsBleedOutTest, "LetsDeepRock.Reinforcement.Gameplay.LeaverClearsBleedOut")
bool FReinforceGameplayLeaverClearsBleedOutTest::RunTest(const FString& Parameters)
{
	ReinforceExpectDeathTrace(*this);
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		GameMode->bDownedRequiresReviver = false;
		GameMode->BleedOutSeconds = 2.f;
		GameMode->AutoReinforceDelay = 30.f;
		ReinforceKill(Ctx);
		Ctx.Test->TestEqual(TEXT("life state is Downed"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Downed);
		Ctx.Test->TestEqual(TEXT("one bleed-out timer pending"), GameMode->GetBleedOutTimerCountForTest(), 1);

		// Logout 调用的清理入口(真实 Logout 本地玩家在 -game 下不安全)
		GameMode->ClearPlayerLifeTimers(Ctx.World->GetFirstPlayerController());
		Ctx.Test->TestEqual(TEXT("bleed-out timer cleared"), GameMode->GetBleedOutTimerCountForTest(), 0);
	});
	ReinforceAddWait(Context, 2.5f);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		Ctx.Test->TestEqual(TEXT("no expiry fired after the clear"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Downed);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

/** 确保本地玩家有稳定身份(-game 下 UniqueId 可能无效),Logout 记录按它为键 */
static void ReinforceEnsurePlayerId(FSwarmGameplayTestContext& Ctx)
{
	if (!Ctx.PlayerState->GetUniqueId().IsValid())
	{
		if (UOnlineEngineInterface* OnlineEngine = UOnlineEngineInterface::Get())
		{
			Ctx.PlayerState->SetUniqueId(FUniqueNetIdRepl(OnlineEngine->CreateUniquePlayerIdWrapper(TEXT("ReinforceRejoinTest"))));
		}
	}
}

/**
 * 模拟"掉线重进":真实 Logout 本地玩家在 -game 下不安全,所以 Record 直接调,
 * 再按重进时的状态还原 PlayerState(新建 PlayerState 默认 Up、满血、无 Dead 标签、旧 Pawn 已销毁),
 * 最后走真实的 RestartPlayer(它会调用 ApplyDepartedLifeState)。
 */
static void ReinforceSimulateRejoin(FSwarmGameplayTestContext& Ctx, AWeaponGameMode* GameMode, AController* Controller)
{
	GameMode->ClearPlayerLifeTimers(Controller);
	if (APawn* OldPawn = Controller->GetPawn())
	{
		OldPawn->Destroy();
	}
	UAbilitySystemComponent* ASC = Ctx.PlayerState->GetAbilitySystemComponent();
	ASC->RemoveLooseGameplayTag(TAG_State_Dead);
	ASC->SetNumericAttributeBase(UWeaponHealthSet::GetHealthAttribute(), 100.f);
	Ctx.PlayerState->SetRespawnServerTime(0.f);
	Ctx.PlayerState->SetLifeStateAuthority(EWeaponPlayerLifeState::Up);
	GameMode->RestartPlayer(Controller);
}

// ---------------------------------------------------------------------------
// I1:局内死亡(池 1)后掉线重进 → 仍是 Dead,重新排自动增援,池不变
// ---------------------------------------------------------------------------
REINFORCE_GAMEPLAY_TEST(FReinforceGameplayRejoinKeepsDeadTest, "LetsDeepRock.Reinforcement.Gameplay.RejoinKeepsDead")
bool FReinforceGameplayRejoinKeepsDeadTest::RunTest(const FString& Parameters)
{
	ReinforceExpectDeathTrace(*this);
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		AController* Controller = Ctx.World->GetFirstPlayerController();
		ReinforceEnsurePlayerId(Ctx);
		GameMode->AutoReinforceDelay = 30.f;
		GameState->SetReinforcementPoolAuthority(1);
		ReinforceKill(Ctx);
		Ctx.Test->TestEqual(TEXT("life state is Dead"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);

		GameMode->RecordDepartedLifeState(Controller);
		Ctx.Test->TestEqual(TEXT("departure recorded"), GameMode->GetDepartedLifeStateCountForTest(), 1);

		ReinforceSimulateRejoin(Ctx, GameMode, Controller);

		Ctx.Test->TestEqual(TEXT("rejoin restores Dead"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);
		Ctx.Test->TestTrue(TEXT("State.Dead tag present"),
			Ctx.PlayerState->GetAbilitySystemComponent()->HasMatchingGameplayTag(TAG_State_Dead));
		Ctx.Test->TestTrue(TEXT("auto-reinforce re-armed"), Ctx.PlayerState->GetRespawnServerTime() > 0.f);
		Ctx.Test->TestEqual(TEXT("pool untouched"), GameState->GetReinforcementPool(), 1);
		Ctx.Test->TestEqual(TEXT("record consumed"), GameMode->GetDepartedLifeStateCountForTest(), 0);
		Ctx.Test->TestEqual(TEXT("mission continues"), GameState->GetMatchPhase(), EWeaponMatchPhase::InGame);
		GameMode->ClearPlayerLifeTimers(Controller);
	});
	ReinforceAddWait(Context, 0.2f);
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// I1:死亡后池耗尽,再掉线重进 → 不排增援;solo 无人存活 → 任务失败
// ---------------------------------------------------------------------------
REINFORCE_GAMEPLAY_TEST(FReinforceGameplayRejoinEmptyPoolFailsTest, "LetsDeepRock.Reinforcement.Gameplay.RejoinEmptyPoolFails")
bool FReinforceGameplayRejoinEmptyPoolFailsTest::RunTest(const FString& Parameters)
{
	ReinforceExpectDeathTrace(*this);
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		AController* Controller = Ctx.World->GetFirstPlayerController();
		ReinforceEnsurePlayerId(Ctx);
		GameMode->AutoReinforceDelay = 30.f;
		GameState->SetReinforcementPoolAuthority(1);
		ReinforceKill(Ctx);
		Ctx.Test->TestEqual(TEXT("life state is Dead"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);

		// 死后池被其他人的增援耗尽;随后该玩家掉线重进
		GameState->SetReinforcementPoolAuthority(0);
		GameMode->RecordDepartedLifeState(Controller);
		ReinforceSimulateRejoin(Ctx, GameMode, Controller);

		Ctx.Test->TestEqual(TEXT("rejoin restores Dead"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);
		Ctx.Test->TestEqual(TEXT("pool still empty"), GameState->GetReinforcementPool(), 0);
		Ctx.Test->TestEqual(TEXT("phase is Ending"), GameState->GetMatchPhase(), EWeaponMatchPhase::Ending);
		Ctx.Test->TestEqual(TEXT("outcome is Failed"), GameState->GetMissionOutcome(), EWeaponMissionOutcome::Failed);
	});
	ReinforceAddWait(Context, ReinforceGameplayTestConstants::EndingPlusRespawnSeconds);
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// M5:TryReinforcePlayer 不得对已存活且有 Pawn 的玩家扣池
// ---------------------------------------------------------------------------
REINFORCE_GAMEPLAY_TEST(FReinforceGameplayReinforceIgnoresUpPlayerTest, "LetsDeepRock.Reinforcement.Gameplay.ReinforceIgnoresUpPlayer")
bool FReinforceGameplayReinforceIgnoresUpPlayerTest::RunTest(const FString& Parameters)
{
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		AController* Controller = Ctx.World->GetFirstPlayerController();
		GameState->SetReinforcementPoolAuthority(3);
		const APawn* PawnBefore = Controller->GetPawn();

		Ctx.Test->TestFalse(TEXT("Up player with a pawn is not reinforced"), GameMode->TryReinforcePlayer(Controller));
		Ctx.Test->TestEqual(TEXT("pool untouched"), GameState->GetReinforcementPool(), 3);
		Ctx.Test->TestTrue(TEXT("pawn not replaced"), Controller->GetPawn() == PawnBefore);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// F1:TryReinforcePlayer 对倒地者同样拒绝,不扣池、不打断流血计时
// ---------------------------------------------------------------------------
REINFORCE_GAMEPLAY_TEST(FReinforceGameplayReinforceIgnoresDownedTest, "LetsDeepRock.Reinforcement.Gameplay.ReinforceIgnoresDowned")
bool FReinforceGameplayReinforceIgnoresDownedTest::RunTest(const FString& Parameters)
{
	ReinforceExpectDeathTrace(*this);
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		GameMode->bDownedRequiresReviver = false;
		GameMode->BleedOutSeconds = 10.f;
		GameMode->AutoReinforceDelay = 30.f;
		GameState->SetReinforcementPoolAuthority(2);
		ReinforceKill(Ctx);
		Ctx.Test->TestEqual(TEXT("life state is Downed"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Downed);

		AController* Controller = Ctx.World->GetFirstPlayerController();
		Ctx.Test->TestFalse(TEXT("Downed player is not reinforced"), GameMode->TryReinforcePlayer(Controller));
		Ctx.Test->TestEqual(TEXT("pool untouched"), GameState->GetReinforcementPool(), 2);
		Ctx.Test->TestEqual(TEXT("still Downed"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Downed);
		Ctx.Test->TestEqual(TEXT("bleed-out timer kept"), GameMode->GetBleedOutTimerCountForTest(), 1);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// F2:结算开始时,已排的 30 s 增援计时必须缩短到 RespawnDelay
// ---------------------------------------------------------------------------
REINFORCE_GAMEPLAY_TEST(FReinforceGameplayEndMatchShortensPendingReinforceTest, "LetsDeepRock.Reinforcement.Gameplay.EndMatchShortensPendingReinforce")
bool FReinforceGameplayEndMatchShortensPendingReinforceTest::RunTest(const FString& Parameters)
{
	ReinforceExpectDeathTrace(*this);
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		GameMode->AutoReinforceDelay = 30.f;
		GameState->SetReinforcementPoolAuthority(2);
		ReinforceKill(Ctx);
		Ctx.Test->TestEqual(TEXT("life state is Dead"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);
		Ctx.Test->TestEqual(TEXT("pending reinforce is about 30 s away"),
			Ctx.PlayerState->GetRespawnServerTime() - ReinforceServerNow(Ctx), 30.f, 0.5f);

		GameMode->EndWeaponMatch(EWeaponMissionOutcome::Extracted);
		Ctx.Test->TestEqual(TEXT("phase is Ending"), GameState->GetMatchPhase(), EWeaponMatchPhase::Ending);
		Ctx.Test->TestTrue(TEXT("pending reinforce shortened to RespawnDelay"),
			Ctx.PlayerState->GetRespawnServerTime() - ReinforceServerNow(Ctx) <= GameMode->GetRespawnDelay() + 0.2f);
	});
	ReinforceAddWait(Context, 4.f);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		Ctx.Test->TestEqual(TEXT("player is Up again"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Up);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// F3:屏障内流血计时暂停,GetBleedOutRemaining 仍给出精确剩余(复制投影 DownedServerTime 会随时间下降)
// ---------------------------------------------------------------------------
REINFORCE_GAMEPLAY_TEST(FReinforceGameplayBleedOutRemainingExactUnderBarrierTest, "LetsDeepRock.Reinforcement.Gameplay.BleedOutRemainingExactUnderBarrier")
bool FReinforceGameplayBleedOutRemainingExactUnderBarrierTest::RunTest(const FString& Parameters)
{
	ReinforceExpectDeathTrace(*this);
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		GameMode->bDownedRequiresReviver = false;
		GameMode->BleedOutSeconds = 10.f;
		GameMode->AutoReinforceDelay = 30.f;
		ReinforceKill(Ctx);
		Ctx.Test->TestEqual(TEXT("life state is Downed"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Downed);
		Ctx.Test->TestTrue(TEXT("barrier entered"), GameMode->EnterGameplayRecoveryBarrier());
		Ctx.ValueA = GameMode->GetBleedOutRemaining(Ctx.World->GetFirstPlayerController());
		Ctx.Test->TestEqual(TEXT("remaining at barrier entry is about 10 s"), static_cast<float>(Ctx.ValueA), 10.f, 0.3f);
	});
	ReinforceAddWait(Context, 3.f);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		Ctx.Test->TestEqual(TEXT("remaining unchanged by 3 s under the barrier"),
			GameMode->GetBleedOutRemaining(Ctx.World->GetFirstPlayerController()), static_cast<float>(Ctx.ValueA), 0.3f);
		Ctx.Test->TestTrue(TEXT("barrier released"), GameMode->ReleaseGameplayRecoveryBarrier());
	});
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		AController* Controller = Ctx.World->GetFirstPlayerController();
		Ctx.Test->TestTrue(TEXT("remaining is still positive after release"), GameMode->GetBleedOutRemaining(Controller) > 0.f);
		GameMode->ClearPlayerLifeTimers(Controller);
		Ctx.Test->TestEqual(TEXT("no timer → 0"), GameMode->GetBleedOutRemaining(Controller), 0.f);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// F6:救援半径由 GameMode 调参并复制到 GameState(客户端据此判定)
// ---------------------------------------------------------------------------
REINFORCE_GAMEPLAY_TEST(FReinforceGameplayReviveRadiusReplicatedTest, "LetsDeepRock.Reinforcement.Gameplay.ReviveRadiusFollowsGameMode")
bool FReinforceGameplayReviveRadiusReplicatedTest::RunTest(const FString& Parameters)
{
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		Ctx.World->GetAuthGameMode<AWeaponGameMode>()->ReviveRadius = 333.f;
	});
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		const AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		const AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		Ctx.Test->TestEqual(TEXT("GameState radius follows the GameMode tunable"), GameState->GetReviveRadius(), GameMode->ReviveRadius);
		Ctx.Test->TestEqual(TEXT("custom value applied"), GameState->GetReviveRadius(), 333.f);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// M2:Ending 阶段快照里的倒地者按死亡恢复并保留待复活计时(不再 0.1 s 流血)
// ---------------------------------------------------------------------------
REINFORCE_GAMEPLAY_TEST(FReinforceGameplaySnapshotEndingDownedRestoresDeadTest, "LetsDeepRock.Reinforcement.Gameplay.SnapshotEndingDownedRestoresDead")
bool FReinforceGameplaySnapshotEndingDownedRestoresDeadTest::RunTest(const FString& Parameters)
{
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		AController* Controller = Ctx.World->GetFirstPlayerController();

		FWeaponPlayerSnapshot Snapshot;
		Snapshot.LifeState = EWeaponPlayerLifeState::Downed;
		Snapshot.bRespawnPending = true;
		Snapshot.RespawnRemainingSeconds = 20.f;
		UWeaponSnapshotCoordinator::ApplyPlayerLifeState(GameMode, Controller, Ctx.PlayerState.Get(), Ctx.Player.Get(),
			Snapshot, EWeaponMatchPhase::Ending);

		Ctx.Test->TestEqual(TEXT("restored as Dead"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);
		Ctx.Test->TestEqual(TEXT("no bleed-out timer"), GameMode->GetBleedOutTimerCountForTest(), 0);
		Ctx.Test->TestEqual(TEXT("pending respawn restored"),
			Ctx.PlayerState->GetRespawnServerTime() - ReinforceServerNow(Ctx), 20.f, 0.3f);
		GameMode->ClearPlayerLifeTimers(Controller);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// 救援:RevivePlayer 恢复 30% 血量、清盾、退出倒地并清流血计时;对 Up 玩家无效
// (按住 E 的双人流程无法在单进程 -game 内跑,由 FWeaponReviveTracker 纯逻辑测试覆盖)
// ---------------------------------------------------------------------------
REINFORCE_GAMEPLAY_TEST(FReinforceGameplayRevivePlayerRestoresTest, "LetsDeepRock.Reinforcement.Gameplay.RevivePlayerRestores")
bool FReinforceGameplayRevivePlayerRestoresTest::RunTest(const FString& Parameters)
{
	ReinforceExpectDeathTrace(*this);
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		GameMode->bDownedRequiresReviver = false;
		GameMode->BleedOutSeconds = 2.f;
		GameMode->AutoReinforceDelay = 30.f;
		ReinforceKill(Ctx);
		Ctx.Test->TestEqual(TEXT("life state is Downed"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Downed);

		// 倒地期间护盾会回复:救起时必须归零;MaxHealth 压回 100。
		UAbilitySystemComponent* ASC = Ctx.PlayerState->GetAbilitySystemComponent();
		ASC->SetNumericAttributeBase(UWeaponHealthSet::GetMaxHealthAttribute(), 100.f);
		ASC->SetNumericAttributeBase(UWeaponHealthSet::GetMaxShieldAttribute(), 50.f);
		ASC->SetNumericAttributeBase(UWeaponHealthSet::GetShieldAttribute(), 25.f);

		AController* Controller = Ctx.World->GetFirstPlayerController();
		GameMode->RevivePlayer(Controller);

		Ctx.Test->TestEqual(TEXT("life state is Up"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Up);
		Ctx.Test->TestEqual(TEXT("health is 30 percent"), ASC->GetNumericAttribute(UWeaponHealthSet::GetHealthAttribute()), 30.f, 0.01f);
		Ctx.Test->TestEqual(TEXT("shield is zero"), ASC->GetNumericAttribute(UWeaponHealthSet::GetShieldAttribute()), 0.f, 0.01f);
		Ctx.Test->TestFalse(TEXT("State.Downed cleared"), ASC->HasMatchingGameplayTag(TAG_State_Downed));
		Ctx.Test->TestEqual(TEXT("downed timestamp cleared"), Ctx.PlayerState->GetDownedServerTime(), 0.f);
		Ctx.Test->TestEqual(TEXT("bleed-out timer cleared"), GameMode->GetBleedOutTimerCountForTest(), 0);
		const AWeaponCharacter* Pawn = Cast<AWeaponCharacter>(Controller->GetPawn());
		Ctx.Test->TestNotNull(TEXT("revived player keeps its pawn"), Pawn);
		if (Pawn)
		{
			Ctx.Test->TestEqual(TEXT("walking again"), Pawn->GetCharacterMovement()->MovementMode.GetValue(), static_cast<uint8>(MOVE_Walking));
		}
	});
	// 流血计时已清:超过 BleedOutSeconds 仍应存活。
	ReinforceAddWait(Context, 2.5f);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		Ctx.Test->TestEqual(TEXT("still Up after the old bleed-out would have expired"),
			Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Up);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

REINFORCE_GAMEPLAY_TEST(FReinforceGameplayReviveIgnoresUpPlayerTest, "LetsDeepRock.Reinforcement.Gameplay.ReviveIgnoresUpPlayer")
bool FReinforceGameplayReviveIgnoresUpPlayerTest::RunTest(const FString& Parameters)
{
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		UAbilitySystemComponent* ASC = Ctx.PlayerState->GetAbilitySystemComponent();
		ASC->SetNumericAttributeBase(UWeaponHealthSet::GetMaxHealthAttribute(), 100.f);
		ASC->SetNumericAttributeBase(UWeaponHealthSet::GetHealthAttribute(), 80.f);
		GameMode->RevivePlayer(Ctx.World->GetFirstPlayerController());
		Ctx.Test->TestEqual(TEXT("health unchanged"), ASC->GetNumericAttribute(UWeaponHealthSet::GetHealthAttribute()), 80.f, 0.01f);
		Ctx.Test->TestEqual(TEXT("still Up"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Up);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// 已阵亡玩家的 Pawn 仍在场内,但不能再为目标点充能
// ---------------------------------------------------------------------------
REINFORCE_GAMEPLAY_TEST(FReinforceGameplayDeadPlayerDoesNotChargeObjectiveTest, "LetsDeepRock.Reinforcement.Gameplay.DeadPlayerDoesNotChargeObjective")
bool FReinforceGameplayDeadPlayerDoesNotChargeObjectiveTest::RunTest(const FString& Parameters)
{
	ReinforceExpectDeathTrace(*this);
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		AWeaponGameState* GameState = Ctx.World->GetGameState<AWeaponGameState>();
		GameMode->AutoReinforceDelay = 30.f;
		GameState->SetReinforcementPoolAuthority(1);

		AGridMapObjectiveActor* Objective = Ctx.World->SpawnActor<AGridMapObjectiveActor>(
			Ctx.Player->GetActorLocation(), FRotator::ZeroRotator);
		if (!Objective)
		{
			Ctx.Fail(TEXT("[Reinforce] could not spawn the objective actor"));
			return;
		}
		Ctx.TrackedActor = Objective;
		ReinforceKill(Ctx);
		Ctx.Test->TestEqual(TEXT("life state is Dead"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);
	});
	ReinforceAddWait(Context, 1.0f);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AGridMapObjectiveActor* Objective = Cast<AGridMapObjectiveActor>(Ctx.TrackedActor.Get());
		if (!Objective)
		{
			Ctx.Fail(TEXT("[Reinforce] objective actor lost"));
			return;
		}
		Ctx.Test->TestEqual(TEXT("dead player does not charge the objective"), Objective->GetChargeProgress(), 0.f);
		Objective->Destroy();
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

// ---------------------------------------------------------------------------
// 快照 v12 恢复:ApplyPlayerLifeState 是 ApplySnapshot 与 RestoreReconnectedPlayer 共用的唯一入口,
// 直接对本地真实玩家调用即同时覆盖两条恢复路径(完整 Coordinator 恢复需要多进程迁移,单进程 -game 无法驱动)。
// ---------------------------------------------------------------------------
REINFORCE_GAMEPLAY_TEST(FReinforceGameplaySnapshotRestoresDownedTest, "LetsDeepRock.Reinforcement.Gameplay.SnapshotRestoresDowned")
bool FReinforceGameplaySnapshotRestoresDownedTest::RunTest(const FString& Parameters)
{
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		AController* Controller = Ctx.World->GetFirstPlayerController();
		Ctx.Test->TestEqual(TEXT("starts Up"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Up);

		FWeaponPlayerSnapshot Snapshot;
		Snapshot.LifeState = EWeaponPlayerLifeState::Downed;
		Snapshot.DownedRemainingSeconds = 4.5f;
		UWeaponSnapshotCoordinator::ApplyPlayerLifeState(GameMode, Controller, Ctx.PlayerState.Get(), Ctx.Player.Get(),
			Snapshot, EWeaponMatchPhase::InGame);

		Ctx.Test->TestEqual(TEXT("restored Downed"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Downed);
		Ctx.Test->TestTrue(TEXT("State.Downed tag"),
			Ctx.PlayerState->GetAbilitySystemComponent()->HasMatchingGameplayTag(TAG_State_Downed));
		Ctx.Test->TestEqual(TEXT("one bleed-out timer"), GameMode->GetBleedOutTimerCountForTest(), 1);
		Ctx.Test->TestEqual(TEXT("remaining matches the snapshot"),
			Ctx.PlayerState->GetDownedServerTime() - ReinforceServerNow(Ctx), 4.5f, 0.3f);

		// 再应用一份 Up 快照:默认值不得保留倒地标签。
		FWeaponPlayerSnapshot UpSnapshot;
		UWeaponSnapshotCoordinator::ApplyPlayerLifeState(GameMode, Controller, Ctx.PlayerState.Get(), Ctx.Player.Get(),
			UpSnapshot, EWeaponMatchPhase::InGame);
		Ctx.Test->TestEqual(TEXT("Up snapshot restores Up"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Up);
		Ctx.Test->TestFalse(TEXT("State.Downed cleared"),
			Ctx.PlayerState->GetAbilitySystemComponent()->HasMatchingGameplayTag(TAG_State_Downed));
		GameMode->ClearPlayerLifeTimers(Controller);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

REINFORCE_GAMEPLAY_TEST(FReinforceGameplaySnapshotRestoresDeadTest, "LetsDeepRock.Reinforcement.Gameplay.SnapshotRestoresDead")
bool FReinforceGameplaySnapshotRestoresDeadTest::RunTest(const FString& Parameters)
{
	FSwarmGameplayTestContextRef Context = SwarmGameplayMakeContext(this);
	SwarmGameplayAddArrange(Context, 300.f);
	ReinforceAddStartMatch(Context);
	SwarmGameplayAddStep(Context, [](FSwarmGameplayTestContext& Ctx)
	{
		AWeaponGameMode* GameMode = Ctx.World->GetAuthGameMode<AWeaponGameMode>();
		AController* Controller = Ctx.World->GetFirstPlayerController();

		// 池空而死:bRespawnPending=false → 保持死亡,不排复活。
		FWeaponPlayerSnapshot Snapshot;
		Snapshot.LifeState = EWeaponPlayerLifeState::Dead;
		Snapshot.bRespawnPending = false;
		UWeaponSnapshotCoordinator::ApplyPlayerLifeState(GameMode, Controller, Ctx.PlayerState.Get(), Ctx.Player.Get(),
			Snapshot, EWeaponMatchPhase::InGame);
		Ctx.Test->TestEqual(TEXT("restored Dead"), Ctx.PlayerState->GetLifeState(), EWeaponPlayerLifeState::Dead);
		Ctx.Test->TestTrue(TEXT("State.Dead tag"),
			Ctx.PlayerState->GetAbilitySystemComponent()->HasMatchingGameplayTag(TAG_State_Dead));
		Ctx.Test->TestEqual(TEXT("no respawn scheduled"), Ctx.PlayerState->GetRespawnServerTime(), 0.f);

		// 等待复活的死者:按快照剩余秒数重排。
		Snapshot.bRespawnPending = true;
		Snapshot.RespawnRemainingSeconds = 20.f;
		UWeaponSnapshotCoordinator::ApplyPlayerLifeState(GameMode, Controller, Ctx.PlayerState.Get(), Ctx.Player.Get(),
			Snapshot, EWeaponMatchPhase::InGame);
		Ctx.Test->TestEqual(TEXT("respawn re-armed from snapshot"),
			Ctx.PlayerState->GetRespawnServerTime() - ReinforceServerNow(Ctx), 20.f, 0.3f);
		GameMode->ClearPlayerLifeTimers(Controller);
	});
	SwarmGameplayAddCleanup(Context);
	return true;
}

#endif // WITH_DEV_AUTOMATION_TESTS
