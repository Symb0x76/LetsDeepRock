// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponGameMode.h"
#include "WeaponCharacter.h"
#include "WeaponPlayerState.h"
#include "Attributes/WeaponHealthSet.h"
#include "AbilitySystemComponent.h"
#include "TimerManager.h"
#include "WeaponGameplayTags.h"
#include "WeaponGameState.h"
#include "Engine/World.h"

AWeaponGameMode::AWeaponGameMode()
{
	GameStateClass = AWeaponGameState::StaticClass();
	PlayerStateClass = AWeaponPlayerState::StaticClass();
}

void AWeaponGameMode::PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId,
	FString& ErrorMessage)
{
	Super::PreLogin(Options, Address, UniqueId, ErrorMessage);
	
	// 保留父类或其他系统已经给出的拒绝原因。
	if (!ErrorMessage.IsEmpty())
	{
		return;
	}
	
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;

	if (!WeaponGameState)
	{
		ErrorMessage = TEXT("Match state is not initialized.");
		return;
	}
	
	if (WeaponGameState->GetConnectedPlayerCount() >= WeaponGameState->GetMaxPlayers())
	{
		ErrorMessage = TEXT("Room is full.");
	}
}

void AWeaponGameMode::PostLogin(APlayerController* NewPlayer)
{
	Super::PostLogin(NewPlayer);
	
	if (!NewPlayer)
	{
		return;
	}
	
	AWeaponPlayerState* WeaponPlayerState = NewPlayer->GetPlayerState<AWeaponPlayerState>();
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;

	if (!WeaponPlayerState || !WeaponGameState)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to initialize player lobby state for %s."), *GetNameSafe(NewPlayer));
		return;
	}
	
	const int32 AvailableSlot = WeaponGameState->FindSmallestAvailablePlayerSlot();
	
	// 理论上 PreLogin 已阻止此情况；保留保护，避免异常连接污染状态。
	if (AvailableSlot == INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("No available player slot for %s."), *GetNameSafe(NewPlayer));
		return;
	}
	
	WeaponPlayerState->SetPlayerSlotIndex(AvailableSlot);
	WeaponPlayerState->SetPlayerReady(false);
	
	RefreshMatchPhase();
}

void AWeaponGameMode::Logout(AController* ExitingController)
{
	// 首先清理 Timer，防止Super::Logout 后，Controller/PlayerState 已被移除或销毁，不适合作为 Timer Map 的查找键
	if (ExitingController)
	{
		if (FTimerHandle* RespawnTimer =
			RespawnTimers.Find(ExitingController))
		{
			GetWorldTimerManager().ClearTimer(*RespawnTimer);
			RespawnTimers.Remove(ExitingController);
		}
	}
	
	Super::Logout(ExitingController);
	
	RefreshMatchPhase();
}

void AWeaponGameMode::PlayerDied(AController* DeadPlayerController)
{
	if (!HasAuthority() || !DeadPlayerController)
	{
		return;
	}
	
	FTimerHandle& RespawnTimer = RespawnTimers.FindOrAdd(DeadPlayerController);
	
	// 同一玩家的重复死亡通知不会创建第二个重生 Timer。
	GetWorldTimerManager().ClearTimer(RespawnTimer);
	
	TWeakObjectPtr<AController> WeakController(DeadPlayerController);
	
	GetWorldTimerManager().SetTimer(
		RespawnTimer, [this, WeakController]
		{
			if (!WeakController.IsValid())
			{
				return;
			}

			AController* PlayerController = WeakController.Get();

			// Timer 已经触发，移除记录，避免 Map 无限积累。
			RespawnTimers.Remove(PlayerController);

			RespawnPlayer(PlayerController);
		},
		RespawnDelay,
		false); // 不循环
}

bool AWeaponGameMode::CanStartWeaponMatch() const
{
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!HasAuthority() || !WeaponGameState || WeaponGameState->GetConnectedPlayerCount() < MinPlayersToStart)
	{
		return false;
	}

	for (const APlayerState* PlayerState : WeaponGameState->PlayerArray)
	{
		if (const AWeaponPlayerState* WeaponPlayerState = Cast<AWeaponPlayerState>(PlayerState); !WeaponPlayerState || !WeaponPlayerState->GetIsPlayerReady())
		{
			return false;
		}
	}

	return true;
}

bool AWeaponGameMode::TryStartWeaponMatch()
{
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!HasAuthority() || !WeaponGameState ||
		WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::Waiting ||
		!CanStartWeaponMatch())
	{
		return false;
	}

	WeaponGameState->SetMatchPhase(EWeaponMatchPhase::Starting, StartingCountdownDuration);

	if (StartingCountdownDuration <= 0.0f)
	{
		BeginWeaponMatch();
	}
	else
	{
		GetWorldTimerManager().SetTimer(
			MatchPhaseTimer,
			this,
			&AWeaponGameMode::BeginWeaponMatch,
			StartingCountdownDuration,
			false);
	}

	return true;
}

void AWeaponGameMode::BeginWeaponMatch()
{
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!WeaponGameState || WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::Starting)
	{
		return;
	}

	if (!CanStartWeaponMatch())
	{
		WeaponGameState->SetMatchPhase(EWeaponMatchPhase::Waiting);
		return;
	}

	WeaponGameState->SetMatchPhase(EWeaponMatchPhase::InGame);
}

void AWeaponGameMode::EndWeaponMatch()
{
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!HasAuthority() || !WeaponGameState || WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::InGame)
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(MatchPhaseTimer);
	WeaponGameState->SetMatchPhase(EWeaponMatchPhase::Ending, EndingDuration);

	if (EndingDuration <= 0.0f)
	{
		ReturnToWaitingForPlayers();
	}
	else
	{
		GetWorldTimerManager().SetTimer(
			MatchPhaseTimer,
			this,
			&AWeaponGameMode::ReturnToWaitingForPlayers,
			EndingDuration,
			false);
	}
}

void AWeaponGameMode::ReturnToWaitingForPlayers()
{
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!WeaponGameState || WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::Ending)
	{
		return;
	}

	ResetAllPlayerReadyStates();
	WeaponGameState->SetMatchPhase(EWeaponMatchPhase::Waiting);
}

void AWeaponGameMode::ResetAllPlayerReadyStates()
{
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!WeaponGameState)
	{
		return;
	}

	for (APlayerState* PlayerState : WeaponGameState->PlayerArray)
	{
		if (AWeaponPlayerState* WeaponPlayerState = Cast<AWeaponPlayerState>(PlayerState))
		{
			WeaponPlayerState->SetPlayerReady(false);
		}
	}
}

void AWeaponGameMode::HandlePlayerReadyChanged()
{
	RefreshMatchPhase();
}

void AWeaponGameMode::RespawnPlayer(AController* PlayerController)
{
	if (!PlayerController)
	{
		return;
	}

	// 1. 销毁旧 Pawn（如果还存在）
	if (APawn* OldPawn = PlayerController->GetPawn())
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
	if (const AWeaponPlayerState* PS = PlayerController->GetPlayerState<AWeaponPlayerState>())
	{
		// 先移除死亡 Tag（在恢复血量之前，避免触发死亡时有 Tag 残留问题）
		if (UAbilitySystemComponent* ASC = PS->GetAbilitySystemComponent())
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

void AWeaponGameMode::RefreshMatchPhase()
{
	AWeaponGameState* WeaponGameState =
		GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;

	if (!WeaponGameState)
	{
		return;
	}
	
	// InGame/Ending 中允许 mid-join；玩家加入或 ready 变化不能让战局倒退。
	if (WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::Starting)
	{
		return;
	}

	// 倒计时中有人取消 ready、掉线或新玩家加入，立即撤销开局。
	if (!CanStartWeaponMatch())
	{
		GetWorldTimerManager().ClearTimer(MatchPhaseTimer);
		WeaponGameState->SetMatchPhase(EWeaponMatchPhase::Waiting);
	}
}
