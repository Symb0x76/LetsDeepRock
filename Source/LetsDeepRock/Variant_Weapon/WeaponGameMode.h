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

	/**
	 *  玩家死亡通知入口（由 WeaponCharacter::HandleHealthDepleted 调用）
	 *  启动重生倒计时，到期后重生玩家。
	 */
	void PlayerDied(AController* DeadPlayerController);

	/**
	 *  立即重生入口（供职业选择 UI 调用）
	 *
	 *  与 PlayerDied 走的延迟重生流程不同：这里跳过 RespawnDelay 倒计时，
	 *  取消任何挂起的死亡重生计时器后立即执行 RespawnPlayer，
	 *  用于"选择职业后立即重生并应用新配置"的场景。
	 *  职业本身的存档写入由调用方（WeaponPlayerController）在此之前完成，
	 *  RespawnPlayer 内部会重新读取存档并应用到新 Pawn。
	 */
	void RespawnPlayerImmediately(AController* PlayerController);

protected:

	/** 重生延迟（秒），可在蓝图子类中配置 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Respawn")
	float RespawnDelay = 3.0f;

	/** 重生计时器句柄（成员变量，支持断线时取消） */
	FTimerHandle RespawnTimerHandle;

	/** 重生玩家：销毁旧 Pawn、生成新 Pawn、恢复状态 */
	void RespawnPlayer(AController* PlayerController);
};
