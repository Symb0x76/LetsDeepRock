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
#include "GameFramework/PlayerController.h"
#include "GameplayEffect.h"
#include "Reinforcement/WeaponReinforcementRules.h"
#include "UObject/Package.h"
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

/** 一击必杀:护盾清零、Health 基础值压到 1,再施加一次瞬时 Damage 效果 */
static void ReinforceKill(FSwarmGameplayTestContext& Ctx)
{
	UAbilitySystemComponent* ASC = Ctx.PlayerState->GetAbilitySystemComponent();
	ASC->SetNumericAttributeBase(UWeaponHealthSet::GetMaxShieldAttribute(), 0.f);
	ASC->SetNumericAttributeBase(UWeaponHealthSet::GetShieldAttribute(), 0.f);
	ASC->SetNumericAttributeBase(UWeaponHealthSet::GetHealthAttribute(), 1.f);

	UGameplayEffect* Effect = NewObject<UGameplayEffect>(GetTransientPackage(), TEXT("ReinforceKillEffect"));
	Effect->DurationPolicy = EGameplayEffectDurationType::Instant;
	FGameplayModifierInfo& Modifier = Effect->Modifiers.AddDefaulted_GetRef();
	Modifier.Attribute = UWeaponHealthSet::GetDamageAttribute();
	Modifier.ModifierOp = EGameplayModOp::Additive;
	Modifier.ModifierMagnitude = FScalableFloat(ReinforceGameplayTestConstants::LethalDamage);
	ASC->ApplyGameplayEffectToSelf(Effect, 1.f, ASC->MakeEffectContext());
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

#endif // WITH_DEV_AUTOMATION_TESTS
