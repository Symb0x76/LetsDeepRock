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

UENUM(BlueprintType)
enum class EWeaponMatchPhase : uint8
{
	/* 等待玩家阶段 */
	WaitingForPlayers = 0 UMETA(DisplayName = "Waiting For Players"),
	
	/* 游戏中 */
	InGame = 1 UMETA(DisplayName = "In Game"),
	
	/* 结算 */
	Ending = 2 UMETA(DisplayName = "Ending"),

	/* 开局倒计时；追加到末尾以保持已有枚举序号稳定 */
	Starting = 3 UMETA(DisplayName = "Starting"),
};

/** 客户端显示战局阶段所需的完整快照。 */
USTRUCT(BlueprintType)
struct FWeaponReplicatedMatchState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Match")
	EWeaponMatchPhase Phase = EWeaponMatchPhase::WaitingForPlayers;

	/** 使用 GameState 的同步服务器时间；0 表示当前阶段没有倒计时。 */
	UPROPERTY(BlueprintReadOnly, Category = "Match")
	float PhaseEndServerTime = 0.0f;
};

// 定义 反射 可多监听 一参数 委托
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(
	FOnWeaponMatchPhaseChanged,	// 委托类型名
	EWeaponMatchPhase,	// 事件携带的数据类型
	NewPhase);	// 参数回调名

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
	
	/** 返回 0–MaxPlayers-1 中最小未占用槽位；满员时返回 INDEX_NONE。仅服务端调用。 */
	int32 FindSmallestAvailablePlayerSlot() const;
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

	/** MatchPhase 在服务器或客户端变化后通知 UI */
	UPROPERTY(BlueprintAssignable, Category = "Match")
	FOnWeaponMatchPhaseChanged OnMatchPhaseChanged;
	
protected:
	
	/** 四人合作上限 */
	UPROPERTY(Replicated, BlueprintReadOnly, Category = "Match", meta = (ClampMin = 1, ClampMax = 4))
	int32 MaxPlayers = 4;
	
	/** 当前阶段及其倒计时作为一个复制快照发送。 */
	UPROPERTY(ReplicatedUsing = OnRep_MatchState, BlueprintReadOnly, Category = "Match")
	FWeaponReplicatedMatchState MatchState;
	
	UFUNCTION()
	void OnRep_MatchState();

private:
	friend class AWeaponGameMode;

	/** 只有服务器 GameMode 可以提交阶段转移。 */
	void SetMatchPhase(EWeaponMatchPhase NewPhase, float PhaseDuration = 0.0f);
};
