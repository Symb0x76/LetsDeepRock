/**
 * @Author: Symb0x76 9667434@qq.com
 * @Date: 2026-07-13 11:37:24
 * @LastEditors: Symb0x76 9667434@qq.com
 * @LastEditTime: 2026-07-13 12:59:54
 * @FilePath: Source/LetsDeepRock/Variant_Weapon/WeaponGameState.h
 */

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameStateBase.h"
#include "WeaponGameState.generated.h"

class AWeaponGameMode;
class UWeaponSessionSubsystem;

UENUM(BlueprintType)
enum class EWeaponMatchPhase : uint8
{
	/* 等待阶段 */
	Waiting = 0 UMETA(DisplayName = "Waiting"),
	
	/* 开局倒计时 */
	Starting = 1 UMETA(DisplayName = "Starting"),
	
	/* 游戏中 */
	InGame = 2 UMETA(DisplayName = "In Game"),
	
	/* 结算 */
	Ending = 3 UMETA(DisplayName = "Ending"),
	
};

/** 任务结果:撤离成功 / 全员倒下且增援池耗尽 */
UENUM(BlueprintType)
enum class EWeaponMissionOutcome : uint8
{
	None = 0,
	Extracted = 1,
	Failed = 2
};

/** 客户端显示战局阶段所需的完整快照。 */
USTRUCT(BlueprintType)
struct FWeaponReplicatedMatchState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Match")
	EWeaponMatchPhase Phase = EWeaponMatchPhase::Waiting;

	/** 使用 GameState 的同步服务器时间；0 表示当前阶段没有倒计时。 */
	UPROPERTY(BlueprintReadOnly, Category = "Match")
	float PhaseEndServerTime = 0.0f;

	/** 本局任务结果;进行中为 None,Ending 阶段为 Extracted/Failed。 */
	UPROPERTY(BlueprintReadOnly, Category = "Match")
	EWeaponMissionOutcome Outcome = EWeaponMissionOutcome::None;
};

/** 一局的目标/撤离进度(A1,客户端 HUD 与大厅可读)。 */
USTRUCT(BlueprintType)
struct FWeaponObjectiveState
{
	GENERATED_BODY()

	/** 目标点是否已充能完成 */
	UPROPERTY(BlueprintReadOnly, Category = "Objective")
	bool bObjectiveComplete = false;

	/** 撤离区是否已开启 */
	UPROPERTY(BlueprintReadOnly, Category = "Objective")
	bool bExtractionOpen = false;

	/** 撤离开启的服务器时间戳;0 = 未排期。客户端用 GetServerWorldTime 换算剩余秒数。 */
	UPROPERTY(BlueprintReadOnly, Category = "Objective")
	float ExtractionReadyServerTime = 0.0f;
};

/** 一局任务的生成参数（由 travel options 或房主在大厅设置，服务器权威）。 */
USTRUCT(BlueprintType)
struct FWeaponMissionSettings
{
	GENERATED_BODY()

	/** 地图生成种子；0 = 沿用 GridMapManager 资产内配置的默认。 */
	UPROPERTY(BlueprintReadOnly, Category = "Mission")
	int32 Seed = 0;

	/** 难度档位：1 低危 / 2 常规 / 3 高危（白盒三档，波次系统消费）。 */
	UPROPERTY(BlueprintReadOnly, Category = "Mission")
	int32 Difficulty = 1;
};

// 定义 反射 可多监听 单参数 委托
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnWeaponMatchPhaseChanged,	// 委托类型名
	EWeaponMatchPhase,	// 事件携带的数据类型
	NewPhase);	// 参数回调名

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnWeaponGameplayRecoveryBarrierChanged,
	bool,
	bGameplayBlocked);

/**
 * 所有客户端共享的战局状态
 *
 * GameMode 仅存在于服务器；需要由客户端读取的战局事实必须放在 GameState
 * 
 * 玩家名单直接使用父类已复制的 PlayerArray，不额外维护重复人数
 */

UCLASS()
class AWeaponGameState : public AGameStateBase
{
	GENERATED_BODY()
	
public:
	AWeaponGameState();
	
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

/** 房间固定最大人数 */
	UFUNCTION(BlueprintPure, Category = "Match")
	int32 GetMaxPlayers() const { return MaxPlayers; }

	/** 本局任务设置（种子/难度）；Waiting 阶段由房主或 travel options 写入。 */
	UFUNCTION(BlueprintPure, Category = "Mission")
	const FWeaponMissionSettings& GetMissionSettings() const { return MissionSettings; }

	/** 当前战局阶段 */
	UFUNCTION(BlueprintPure, Category = "Match")
	EWeaponMatchPhase GetMatchPhase() const { return MatchState.Phase; }

	/** 当前阶段剩余秒数；没有倒计时时返回 0。 */
	UFUNCTION(BlueprintPure, Category = "Match")
	float GetRemainingPhaseTime() const;

	/** 已连接玩家数量；PlayerArray 由引擎负责复制 */
	UFUNCTION(BlueprintPure, Category = "Match")
	int32 GetConnectedPlayerCount() const;

	/** RecoveryReady 发布前为 true；UI 和本地输入读取同一个复制事实。 */
	UFUNCTION(BlueprintPure, Category = "Recovery")
	bool IsGameplayRecoveryBlocked() const { return bGameplayRecoveryBlocked; }

	/** Low-level gameplay owners use this without depending on GameMode. */
	static bool IsGameplayRecoveryBlockedFor(const UObject* WorldContextObject);

	/** 团队共享的增援池(剩余可复活次数) */
	UFUNCTION(BlueprintPure, Category = "Reinforcement")
	int32 GetReinforcementPool() const { return ReinforcementPool; }

	/** 本局任务结果 */
	UFUNCTION(BlueprintPure, Category = "Match")
	EWeaponMissionOutcome GetMissionOutcome() const { return MatchState.Outcome; }

	/** 服务器权威写入增援池(夹到 >= 0)。 */
	void SetReinforcementPoolAuthority(int32 NewPool);

	/** 目标/撤离进度(A1) */
	UFUNCTION(BlueprintPure, Category = "Objective")
	const FWeaponObjectiveState& GetObjectiveState() const { return ObjectiveState; }

	/** MatchPhase 在服务器或客户端变化后通知 UI */
	UPROPERTY(BlueprintAssignable, Category = "Match")
	FOnWeaponMatchPhaseChanged OnMatchPhaseChanged;

	UPROPERTY(BlueprintAssignable, Category = "Recovery")
	FOnWeaponGameplayRecoveryBarrierChanged OnGameplayRecoveryBarrierChanged;
	
protected:
	
	/** 四人合作上限 */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Match", meta = (ClampMin = 1, ClampMax = 4))
	int32 MaxPlayers = 4;
	
	UPROPERTY(ReplicatedUsing = OnRep_MissionSettings, BlueprintReadOnly, Category = "Mission")
	FWeaponMissionSettings MissionSettings;
	
	/** 当前阶段及其倒计时作为一个复制快照发送。 */
	UPROPERTY(ReplicatedUsing = OnRep_MatchState, BlueprintReadOnly, Category = "Match")
	FWeaponReplicatedMatchState MatchState;

	/** 团队共享的增援池;开局按难度初始化,每次自动增援复活扣 1。 */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Reinforcement")
	int32 ReinforcementPool = 0;

	UPROPERTY(ReplicatedUsing = OnRep_GameplayRecoveryBlocked, BlueprintReadOnly, Category = "Recovery")
	bool bGameplayRecoveryBlocked = false;
	UPROPERTY(ReplicatedUsing = OnRep_ObjectiveState, BlueprintReadOnly, Category = "Objective")
	FWeaponObjectiveState ObjectiveState;
	
	
	UFUNCTION()
	void OnRep_MissionSettings() const;
	
	UFUNCTION()
	void OnRep_MatchState() const;

	UFUNCTION()
	void OnRep_GameplayRecoveryBlocked() const;
	UFUNCTION()
	void OnRep_ObjectiveState() const;
	

	/** 服务器权威写入任务设置；内部校验权威/阶段（Waiting|Starting）。 */
public:
	bool SetMissionSettingsAuthority(const FWeaponMissionSettings& NewSettings);

private:
	friend class AWeaponGameMode;
	friend class UWeaponSnapshotCoordinator;
	friend class UWeaponSessionSubsystem;
	void SetMatchPhase(EWeaponMatchPhase NewPhase, float PhaseDuration = 0.0f);
	/** 服务器写入任务结果(GameMode 在结束/重置时调用)。 */
	void SetMissionOutcomeAuthority(EWeaponMissionOutcome NewOutcome);
	/** Snapshot 恢复专用:屏障内写回增援池。 */
	void RestoreReinforcementPoolForRecovery(int32 RestoredPool);
	bool SetMaxPlayersAuthority(int32 NewMaxPlayers);
	bool SetGameplayRecoveryBlockedAuthority(bool bBlocked);
	/** Snapshot v10 恢复专用:屏障内绕过 Waiting/Starting 阶段校验写回任务设置。 */
	void RestoreMissionSettingsForRecovery(const FWeaponMissionSettings& RestoredSettings);

	/** 服务器权威写入目标/撤离进度(供 GameMode 的 A1 流程调用)。 */
public:
	void SetObjectiveStateAuthority(const FWeaponObjectiveState& NewState);

private:
};
