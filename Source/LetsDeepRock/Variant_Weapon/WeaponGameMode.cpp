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

bool AWeaponGameMode::SetPlayerSlotReservations(
	const TMap<FUniqueNetIdRepl, int32>& NewReservations)
{
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!HasAuthority() || !WeaponGameState)
	{
		return false;
	}

	TSet<int32> ReservedSlots;
	for (const auto& [PlayerId, SlotIndex] : NewReservations)
	{
		if (!PlayerId.IsValid() || SlotIndex < 0 || SlotIndex >= WeaponGameState->GetMaxPlayers() ||
			ReservedSlots.Contains(SlotIndex))
		{
			return false;
		}
		ReservedSlots.Add(SlotIndex);
	}

	TSet<FUniqueNetIdRepl> ConnectedPlayerIds;
	for (const APlayerState* PlayerState : WeaponGameState->PlayerArray)
	{
		const AWeaponPlayerState* WeaponPlayerState = Cast<AWeaponPlayerState>(PlayerState);
		if (!WeaponPlayerState || !NewReservations.Contains(WeaponPlayerState->GetUniqueId()))
		{
			return false;
		}
		ConnectedPlayerIds.Add(WeaponPlayerState->GetUniqueId());
	}

	for (auto& TimerPair : PendingGraceTimers)
	{
		GetWorldTimerManager().ClearTimer(TimerPair.Value);
	}
	PendingGraceTimers.Reset();

	PlayerSlotReservations = NewReservations;
	RecoveryRoster.Reset();
	const double CurrentServerTime = GetWorld()->GetTimeSeconds();
	TArray<FUniqueNetIdRepl> PendingPlayerIds;
	for (const auto& [PlayerId, SlotIndex] : PlayerSlotReservations)
	{
		FWeaponRecoveryRosterEntry& Entry = RecoveryRoster.Add(PlayerId);
		Entry.PlayerId = PlayerId;
		Entry.OriginalSlot = SlotIndex;
		if (ConnectedPlayerIds.Contains(PlayerId))
		{
			Entry.Status = EWeaponRecoveryPlayerStatus::Ready;
		}
		else
		{
			Entry.Status = EWeaponRecoveryPlayerStatus::Pending;
			Entry.PendingDeadlineServerTime = CurrentServerTime + PendingGracePeriodSeconds;
			PendingPlayerIds.Add(PlayerId);
		}
	}
	++RosterRevision;

	for (const FUniqueNetIdRepl& PlayerId : PendingPlayerIds)
	{
		MarkPlayerPending(PlayerId);
	}
	return true;
}

void AWeaponGameMode::ClearPendingGraceTimer(const FUniqueNetIdRepl& PlayerId)
{
	if (FTimerHandle* TimerHandle = PendingGraceTimers.Find(PlayerId))
	{
		GetWorldTimerManager().ClearTimer(*TimerHandle);
		PendingGraceTimers.Remove(PlayerId);
	}
}

void AWeaponGameMode::MarkPlayerReady(const FUniqueNetIdRepl& PlayerId, const int32 OriginalSlot)
{
	ClearPendingGraceTimer(PlayerId);

	FWeaponRecoveryRosterEntry* Entry = RecoveryRoster.Find(PlayerId);
	const bool bRosterChanged = !Entry || Entry->Status != EWeaponRecoveryPlayerStatus::Ready ||
		Entry->OriginalSlot != OriginalSlot;
	if (!Entry)
	{
		Entry = &RecoveryRoster.Add(PlayerId);
	}

	Entry->PlayerId = PlayerId;
	Entry->OriginalSlot = OriginalSlot;
	Entry->Status = EWeaponRecoveryPlayerStatus::Ready;
	Entry->PendingDeadlineServerTime = 0.0;
	PlayerSlotReservations.Add(PlayerId, OriginalSlot);
	if (bRosterChanged)
	{
		++RosterRevision;
	}
}

void AWeaponGameMode::MarkPlayerPending(const FUniqueNetIdRepl& PlayerId)
{
	FWeaponRecoveryRosterEntry* Entry = RecoveryRoster.Find(PlayerId);
	const int32* OriginalSlot = PlayerSlotReservations.Find(PlayerId);
	if (!Entry || !OriginalSlot)
	{
		return;
	}

	if (Entry->Status != EWeaponRecoveryPlayerStatus::Pending)
	{
		Entry->Status = EWeaponRecoveryPlayerStatus::Pending;
		++RosterRevision;
	}
	Entry->PendingDeadlineServerTime = GetWorld()->GetTimeSeconds() + PendingGracePeriodSeconds;

	ClearPendingGraceTimer(PlayerId);
	if (PendingGracePeriodSeconds <= 0.0f)
	{
		HandlePendingGraceExpired(PlayerId);
		return;
	}

	FTimerDelegate ExpirationDelegate;
	ExpirationDelegate.BindUObject(this, &AWeaponGameMode::HandlePendingGraceExpired, PlayerId);
	FTimerHandle& TimerHandle = PendingGraceTimers.Add(PlayerId);
	GetWorldTimerManager().SetTimer(
		TimerHandle,
		ExpirationDelegate,
		PendingGracePeriodSeconds,
		false);
}

bool AWeaponGameMode::MarkPlayerDeparted(const FUniqueNetIdRepl& PlayerId)
{
	FWeaponRecoveryRosterEntry* Entry = RecoveryRoster.Find(PlayerId);
	if (!Entry || Entry->Status == EWeaponRecoveryPlayerStatus::Departed)
	{
		return false;
	}

	ClearPendingGraceTimer(PlayerId);
	Entry->Status = EWeaponRecoveryPlayerStatus::Departed;
	Entry->PendingDeadlineServerTime = 0.0;
	PlayerSlotReservations.Remove(PlayerId);
	++RosterRevision;
	return true;
}

void AWeaponGameMode::HandlePendingGraceExpired(FUniqueNetIdRepl PlayerId)
{
	PendingGraceTimers.Remove(PlayerId);
	FWeaponRecoveryRosterEntry* Entry = RecoveryRoster.Find(PlayerId);
	if (!Entry || Entry->Status != EWeaponRecoveryPlayerStatus::Pending)
	{
		return;
	}

	MarkPlayerDeparted(PlayerId);
}

bool AWeaponGameMode::IsPlayerIdentityConnected(const FUniqueNetIdRepl& PlayerId) const
{
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!WeaponGameState || !PlayerId.IsValid())
	{
		return false;
	}

	for (const APlayerState* PlayerState : WeaponGameState->PlayerArray)
	{
		if (PlayerState && PlayerState->GetUniqueId() == PlayerId)
		{
			return true;
		}
	}
	return false;
}

int32 AWeaponGameMode::FindSmallestUnreservedPlayerSlot() const
{
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!WeaponGameState)
	{
		return INDEX_NONE;
	}

	for (int32 SlotIndex = 0; SlotIndex < WeaponGameState->GetMaxPlayers(); ++SlotIndex)
	{
		if (!PlayerSlotReservations.FindKey(SlotIndex))
		{
			return SlotIndex;
		}
	}
	return INDEX_NONE;
}

int32 AWeaponGameMode::FindOrReservePlayerSlot(const FUniqueNetIdRepl& PlayerId)
{
	if (!PlayerId.IsValid())
	{
		return INDEX_NONE;
	}
	if (const int32* ReservedSlot = PlayerSlotReservations.Find(PlayerId))
	{
		return *ReservedSlot;
	}

	const int32 AvailableSlot = FindSmallestUnreservedPlayerSlot();
	if (AvailableSlot != INDEX_NONE)
	{
		PlayerSlotReservations.Add(PlayerId, AvailableSlot);
	}
	return AvailableSlot;
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
	if (!UniqueId.IsValid())
	{
		ErrorMessage = TEXT("A stable online player identity is required.");
		return;
	}
	if (IsPlayerIdentityConnected(UniqueId))
	{
		ErrorMessage = TEXT("This player identity is already connected.");
		return;
	}
	if (const FWeaponRecoveryRosterEntry* Entry = RecoveryRoster.Find(UniqueId);
		Entry && Entry->Status == EWeaponRecoveryPlayerStatus::Departed)
	{
		ErrorMessage = TEXT("This player has departed from the recoverable match roster.");
		return;
	}
	
	if (WeaponGameState->GetConnectedPlayerCount() >= WeaponGameState->GetMaxPlayers())
	{
		ErrorMessage = TEXT("Room is full.");
		return;
	}
	if (!PlayerSlotReservations.Contains(UniqueId) && FindSmallestUnreservedPlayerSlot() == INDEX_NONE)
	{
		ErrorMessage = TEXT("All player slots are reserved.");
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
	
	const FUniqueNetIdRepl& PlayerId = WeaponPlayerState->GetUniqueId();
	const int32 AvailableSlot = FindOrReservePlayerSlot(PlayerId);
	
	// 理论上 PreLogin 已阻止此情况；保留保护，避免异常连接污染状态。
	if (AvailableSlot == INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("No available player slot for %s (%s)."),
			*GetNameSafe(NewPlayer), *PlayerId.ToDebugString());
		return;
	}
	
	WeaponPlayerState->SetPlayerSlotIndex(AvailableSlot);
	WeaponPlayerState->SetPlayerReady(false);
	MarkPlayerReady(PlayerId, AvailableSlot);
	
	RefreshMatchPhase();
}

void AWeaponGameMode::Logout(AController* ExitingController)
{
	FUniqueNetIdRepl ExitingPlayerId;
	if (ExitingController)
	{
		if (const APlayerState* PlayerState = ExitingController->GetPlayerState<APlayerState>())
		{
			ExitingPlayerId = PlayerState->GetUniqueId();
		}
	}

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
	if (ExitingPlayerId.IsValid())
	{
		MarkPlayerPending(ExitingPlayerId);
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
