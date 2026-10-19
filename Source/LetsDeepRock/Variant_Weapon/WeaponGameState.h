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
	
	/** 当前阶段及其倒计时作为一个复制快照发送。 */
	UPROPERTY(ReplicatedUsing = OnRep_MatchState, BlueprintReadOnly, Category = "Match")
	FWeaponReplicatedMatchState MatchState;

	UPROPERTY(ReplicatedUsing = OnRep_GameplayRecoveryBlocked, BlueprintReadOnly, Category = "Recovery")
	bool bGameplayRecoveryBlocked = false;
	UPROPERTY(ReplicatedUsing = OnRep_ObjectiveState, BlueprintReadOnly, Category = "Objective")
	FWeaponObjectiveState ObjectiveState;
	
	
	UFUNCTION()
	void OnRep_MatchState() const;

	UFUNCTION()
	void OnRep_GameplayRecoveryBlocked() const;
	UFUNCTION()
	void OnRep_ObjectiveState() const;
	

private:
	friend class AWeaponGameMode;
	friend class UWeaponSnapshotCoordinator;
	friend class UWeaponSessionSubsystem;
	void SetMatchPhase(EWeaponMatchPhase NewPhase, float PhaseDuration = 0.0f);
	bool SetMaxPlayersAuthority(int32 NewMaxPlayers);
	bool SetGameplayRecoveryBlockedAuthority(bool bBlocked);

	/** 服务器权威写入目标/撤离进度(供 GameMode 的 A1 流程调用)。 */
public:
	void SetObjectiveStateAuthority(const FWeaponObjectiveState& NewState);

private:
};
