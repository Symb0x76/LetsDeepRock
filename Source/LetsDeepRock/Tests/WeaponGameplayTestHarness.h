// 关卡内玩法自动化测试的共享测试框架（原 SwarmGameplayTests.cpp 内的 static 辅助，原样搬出；须以 -game 运行）

#pragma once

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Engine/World.h"
#include "Swarm/SwarmBackend.h"
#include "Swarm/SwarmBugPawn.h"
#include "Swarm/SwarmSubsystem.h"
#include "WeaponCharacter.h"
#include "WeaponPlayerState.h"

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

void SwarmGameplayEnqueue(TFunction<bool()> Predicate);
UWorld* SwarmGameplayFindGameWorld();
void SwarmGameplayAddStep(const FSwarmGameplayTestContextRef& Context, TFunction<void(FSwarmGameplayTestContext&)> Step);
void SwarmGameplayAddWorldWait(const FSwarmGameplayTestContextRef& Context, const float Seconds,
	TFunction<bool(FSwarmGameplayTestContext&, float)> PerFrame = nullptr);
void SwarmGameplayAddOpenMap(const FSwarmGameplayTestContextRef& Context, const TCHAR* MapPath);
void SwarmGameplayRemoveMapDelegate(FSwarmGameplayTestContext& Context);
double SwarmGameplayMeasureOpenDirection(UWorld& World, const FVector& Origin, const AActor* IgnoredActor,
	const float RequiredDistance, FVector& OutDirection);
bool SwarmGameplayChooseArena(FSwarmGameplayTestContext& Context, const float RequiredDistance);
bool SwarmGameplayFindOpenDirection(FSwarmGameplayTestContext& Context, const float RequiredDistance);
void SwarmGameplayAddArrange(const FSwarmGameplayTestContextRef& Context, const float RequiredOpenDistance);
void SwarmGameplayAddCleanup(const FSwarmGameplayTestContextRef& Context);
FSwarmGameplayTestContextRef SwarmGameplayMakeContext(FAutomationTestBase* Test);
void SwarmGameplayCheck(FSwarmGameplayTestContext& Context, const FString& What, const bool bValue);

#endif // WITH_DEV_AUTOMATION_TESTS
