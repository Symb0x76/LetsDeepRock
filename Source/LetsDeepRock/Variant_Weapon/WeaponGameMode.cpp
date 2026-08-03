// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponGameMode.h"
#include "WeaponCharacter.h"
#include "WeaponPlayerState.h"
#include "Attributes/WeaponHealthSet.h"
#include "AbilitySystemComponent.h"
#include "GameFramework/PlayerStart.h"
#include "TimerManager.h"
#include "WeaponGameplayTags.h"
#include "WeaponHUD.h"

AWeaponGameMode::AWeaponGameMode()
{
	// 默认使用 AWeaponHUD 绘制当前武器调试信息；
	// 若 BP_WeaponGameMode 在编辑器中显式指定了其他 HUD Class，会覆盖此默认值。
	HUDClass = AWeaponHUD::StaticClass();
}

void AWeaponGameMode::PlayerDied(AController* DeadPlayerController)
{
	if (!DeadPlayerController)
	{
		return;
	}

	// 启动重生计时器（使用成员 Handle，便于后续取消；如需支持多玩家需改为 TMap）
	// 使用 WeakObjectPtr 防止玩家断线后 Controller 已销毁导致悬垂指针
	TWeakObjectPtr<AController> WeakController(DeadPlayerController);
	GetWorldTimerManager().SetTimer(
		RespawnTimerHandle,
		[this, WeakController]()
		{
			if (WeakController.IsValid())
			{
				RespawnPlayer(WeakController.Get());
			}
		},
		RespawnDelay,
		false // 不循环
	);
}

void AWeaponGameMode::RespawnPlayer(AController* PlayerController)
{
	if (!PlayerController)
	{
		return;
	}

	// 1. 销毁旧 Pawn（如果还存在）
	APawn* OldPawn = PlayerController->GetPawn();
	if (OldPawn)
	{
		OldPawn->Destroy();
	}

	// 2. 寻找重生点
	AActor* StartSpot = FindPlayerStart(PlayerController);

	// 3. 生成新 Pawn（使用 GameMode 配置的 DefaultPawnClass）
	APawn* NewPawn = SpawnDefaultPawnFor(PlayerController, StartSpot);
	if (!NewPawn)
	{
		return;
	}

	// 4. 让 Controller 持有新 Pawn
	// Possess 会触发 WeaponCharacter::PossessedBy → InitAbilitySystem → 装备武器
	PlayerController->Possess(NewPawn);

	// 5. 恢复玩家状态（Possess 之后，ASC ActorInfo 已更新到新 Pawn）
	AWeaponPlayerState* PS = PlayerController->GetPlayerState<AWeaponPlayerState>();
	if (PS)
	{
		// 先移除死亡 Tag（在恢复血量之前，避免触发死亡时有 Tag 残留问题）
		UAbilitySystemComponent* ASC = PS->GetAbilitySystemComponent();
		if (ASC)
		{
			ASC->RemoveLooseGameplayTag(TAG_State_Dead);
		}

		// 恢复满血
		if (UWeaponHealthSet* HealthSet = PS->GetHealthSet())
		{
			HealthSet->SetHealth(HealthSet->GetMaxHealth());
		}
	}
}
