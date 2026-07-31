// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/OnlineReplStructs.h"
#include "WeaponGameMode.generated.h"

class AWeaponCharacter;

/**
 *  俯视角武器系统 Game Mode（Variant_Weapon）
 *
 *  职责：
 *  - 指定默认 Pawn / Controller
 *  - 玩家死亡 → 重生流程管理（阶段 5）
 */
UCLASS(abstract)
class AWeaponGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:

	AWeaponGameMode();

	virtual void PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage) override;

	virtual void PostLogin(APlayerController* NewPlayer) override;

	virtual void Logout(AController* ExitingController) override;

	/** Replace the server-only stable identity -> original Slot table after validating the whole set. */
	bool SetPlayerSlotReservations(const TMap<FUniqueNetIdRepl, int32>& NewReservations);
	
	/**
	 *  玩家死亡通知入口（由 WeaponCharacter::HandleHealthDepleted 调用）
	 *  启动重生倒计时，到期后重生玩家。
	 */
	void PlayerDied(AController* DeadPlayerController);

	/** 所有玩家 ready 且人数达标时，由 Listen Host 发起开局倒计时。 */
	UFUNCTION(BlueprintCallable, Category = "Match")
	bool TryStartWeaponMatch();

	/** 由服务端胜负/任务逻辑调用，进入结算阶段。 */
	UFUNCTION(BlueprintCallable, Category = "Match")
	void EndWeaponMatch();

	/** PlayerState ready 改变后的服务器通知入口。 */
	void HandlePlayerReadyChanged();

	UFUNCTION(BlueprintPure, Category = "Match")
	bool CanStartWeaponMatch() const;

protected:

	/** 重生延迟（秒），可在蓝图子类中配置 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Respawn")
	float RespawnDelay = 3.0f;

	/** 最少多少名玩家 ready 才能开始；允许 1~4 人合作测试。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Match", meta = (ClampMin = 1, ClampMax = 4))
	int32 MinPlayersToStart = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Match", meta = (ClampMin = 0.0))
	float StartingCountdownDuration = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Match", meta = (ClampMin = 0.0))
	float EndingDuration = 5.0f;
	
	/**
	* 每个 Controller 独立拥有一个重生计时器。
	* Logout 时必须 ClearTimer 并 Remove，避免持有已断线 Controller。
	*/
	TMap<TObjectPtr<AController>, FTimerHandle> RespawnTimers;
	
	/** 重生玩家：销毁旧 Pawn、生成新 Pawn、恢复状态 */
	void RespawnPlayer(AController* PlayerController);

private:
	bool IsPlayerIdentityConnected(const FUniqueNetIdRepl& PlayerId) const;
	int32 FindSmallestUnreservedPlayerSlot() const;
	int32 FindOrReservePlayerSlot(const FUniqueNetIdRepl& PlayerId);

	/** Reservations outlive a connection; Commit 3 adds Pending/Ready/Departed expiry policy. */
	TMap<FUniqueNetIdRepl, int32> PlayerSlotReservations;

	/** 玩家加入、退出或 ready 改变时重新验证当前阶段。 */
	void RefreshMatchPhase();

	void BeginWeaponMatch();
	void ReturnToWaitingForPlayers();
	void ResetAllPlayerReadyStates();

	FTimerHandle MatchPhaseTimer;
};
