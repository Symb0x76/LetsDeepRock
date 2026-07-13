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

UENUM(BlueprintType)
enum class EWeaponMatchPhase : uint8
{
	/* 等待玩家阶段 */
	WaitingForPlayers UMETA(DisplayName = "Waiting For Players"),
	
	/* 游戏中 */
	InGame UMETA(DisplayName = "In Game"),
	
	/* 结算 */
	Ending UMETA(DisplayName = "Ending"),
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
	/**
	* 仅服务端调用
	* 
	* 后续由 AWeaponGameMode 在 PostLogin、Logout、开始/结束战局时更新
	*/
	void SetMatchPhase(EWeaponMatchPhase NewPhase);
	
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

/** 房间固定最大人数 */
	UFUNCTION(BlueprintPure, Category = "Match")
	int32 GetMaxPlayers() const { return MaxPlayers; }

	/** 当前战局阶段 */
	UFUNCTION(BlueprintPure, Category = "Match")
	EWeaponMatchPhase GetMatchPhase() const { return MatchPhase; }

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
	
	/** 当前战局阶段 */
	UPROPERTY(ReplicatedUsing = OnRep_MatchPhase, BlueprintReadOnly, Category = "Match")
	EWeaponMatchPhase MatchPhase = EWeaponMatchPhase::WaitingForPlayers;
	
	UFUNCTION()
	void OnRep_MatchPhase();
};