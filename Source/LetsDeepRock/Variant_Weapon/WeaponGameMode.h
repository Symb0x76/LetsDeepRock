// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
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
	
	/**
	 *  玩家死亡通知入口（由 WeaponCharacter::HandleHealthDepleted 调用）
	 *  启动重生倒计时，到期后重生玩家。
	 */
	void PlayerDied(AController* DeadPlayerController);

protected:

	/** 重生延迟（秒），可在蓝图子类中配置 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Respawn")
	float RespawnDelay = 3.0f;
	
	/**
	* 每个 Controller 独立拥有一个重生计时器。
	* Logout 时必须 ClearTimer 并 Remove，避免持有已断线 Controller。
	*/
	TMap<TObjectPtr<AController>, FTimerHandle> RespawnTimers;
	
	/** 重生玩家：销毁旧 Pawn、生成新 Pawn、恢复状态 */
	void RespawnPlayer(AController* PlayerController);

private:

	/** Feature 0 始终设为 WaitingForPlayers；后续 feature 再加入自动开局。 */
	void RefreshMatchPhase();
};
