// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponGameMode.h"
#include "LetsDeepRock.h"
#include "WeaponCharacter.h"
#include "NPCBase.h"
#include "Drone.h"
#include "AI/EnemyAIController.h"
#include "WeaponPlayerController.h"
#include "WeaponPlayerState.h"
#include "Weapons/WeaponProjectile.h"
#include "Attributes/WeaponHealthSet.h"
#include "AbilitySystemComponent.h"
#include "TimerManager.h"
#include "WeaponGameplayTags.h"
#include "WeaponGameState.h"
#include "Online/WeaponHostMigrationSubsystem.h"
#include "Online/WeaponSnapshotCoordinator.h"
#include "Online/WeaponSnapshot.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GridMap/GridMapManager.h"
#include "GridMap/GridMapObjectiveActors.h"
#include "EngineUtils.h"
#include "AI/WeaponWaveDirectorComponent.h"
#include "Kismet/GameplayStatics.h"
#include "UI/WeaponHUD.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerStart.h"
#include "Reinforcement/WeaponReinforcementRules.h"

namespace
{
constexpr float WeaponFallbackSpawnSpacing = 250.0f;
}

AWeaponGameMode::AWeaponGameMode()
{
	bUseSeamlessTravel = true;
	HUDClass = AWeaponHUD::StaticClass();

	ObjectiveActorClass = AGridMapObjectiveActor::StaticClass();
	ExtractionActorClass = AGridMapExtractionActor::StaticClass();
	WaveDirector = CreateDefaultSubobject<UWeaponWaveDirectorComponent>(TEXT("WaveDirector"));
	GameStateClass = AWeaponGameState::StaticClass();
	PlayerStateClass = AWeaponPlayerState::StaticClass();
}

void AWeaponGameMode::InitGame(
	const FString& MapName,
	const FString& Options,
	FString& ErrorMessage)
{
	bRecoveryWorldRequested = UGameplayStatics::HasOption(Options, TEXT("WeaponRecovery"));
	if (!bRecoveryWorldRequested)
	{
		// 任务参数（A0）：单人任务页 / 大厅房主设置随 travel options 带入。
		// Seed=0 表示未指定（沿用资产默认）；难度白盒三档。
		ParsedMissionSettings.Seed = UGameplayStatics::GetIntOption(Options, TEXT("Seed"), 0);
		ParsedMissionSettings.Difficulty = FMath::Clamp(
			UGameplayStatics::GetIntOption(Options, TEXT("Difficulty"), 1), 1, 3);
		bArrivedFromLobby = UGameplayStatics::HasOption(Options, TEXT("FromLobby"));
		ExpectedLobbyPlayers = FMath::Clamp(UGameplayStatics::GetIntOption(Options, TEXT("LobbyPlayers"), 1), 1, 4);
	}
	Super::InitGame(MapName, Options, ErrorMessage);
}

void AWeaponGameMode::InitGameState()
{
	Super::InitGameState();
	if (AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr)
	{
		WeaponGameState->SetMissionSettingsAuthority(ParsedMissionSettings);
	}
	if (bRecoveryWorldRequested && !EnterGameplayRecoveryBarrier())
	{
		UE_LOG(LogLetsDeepRock, Fatal, TEXT("Recovery World could not enter its gameplay barrier during InitGameState"));
	}
}


bool AWeaponGameMode::RefreshHostMigrationRoles(FString& OutError)
{
	OutError.Reset();
	UWorld* World = GetWorld();
	AWeaponGameState* WeaponGameState = World ? World->GetGameState<AWeaponGameState>() : nullptr;
	if (!HasAuthority() || !World || !WeaponGameState)
	{
		OutError = TEXT("Migration roles require an authoritative Weapon server World");
		return false;
	}
	if (World->GetNetMode() != NM_ListenServer)
	{
		return true;
	}
	if (WeaponGameState->IsGameplayRecoveryBlocked())
	{
		return true;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UWeaponHostMigrationSubsystem* MigrationSubsystem =
		GameInstance ? GameInstance->GetSubsystem<UWeaponHostMigrationSubsystem>() : nullptr;
	UWeaponSnapshotCoordinator* SnapshotCoordinator =
		GameInstance ? GameInstance->GetSubsystem<UWeaponSnapshotCoordinator>() : nullptr;
	if (!MigrationSubsystem || !SnapshotCoordinator)
	{
		OutError = TEXT("Migration role or Snapshot subsystem is unavailable");
		return false;
	}

	struct FConnectedMigrationMember
	{
		FWeaponMigrationRoleMember Member;
		AWeaponPlayerController* Controller = nullptr;
	};

	FConnectedMigrationMember ActiveHost;
	TArray<FConnectedMigrationMember> BackupCandidates;
	for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
	{
		AWeaponPlayerController* Controller = Cast<AWeaponPlayerController>(It->Get());
		AWeaponPlayerState* PlayerState = Controller
			? Controller->GetPlayerState<AWeaponPlayerState>()
			: nullptr;
		const FUniqueNetIdRepl PlayerId = PlayerState
			? PlayerState->GetUniqueId()
			: FUniqueNetIdRepl();
		const FWeaponRecoveryRosterEntry* RosterEntry = RecoveryRoster.Find(PlayerId);
		if (!Controller || !PlayerState || !PlayerId.IsValid() || !RosterEntry ||
			RosterEntry->Status != EWeaponRecoveryPlayerStatus::Ready ||
			RosterEntry->OriginalSlot != PlayerState->GetPlayerSlotIndex())
		{
			continue;
		}

		FConnectedMigrationMember ConnectedMember;
		ConnectedMember.Member.PlayerId = PlayerId;
		ConnectedMember.Member.OriginalSlot = RosterEntry->OriginalSlot;
		ConnectedMember.Controller = Controller;
		if (Controller->IsLocalController())
		{
			if (ActiveHost.Controller)
			{
				OutError = TEXT("Server World contains more than one local Active Host controller");
				return false;
			}
			ActiveHost = MoveTemp(ConnectedMember);
		}
		else
		{
			BackupCandidates.Add(MoveTemp(ConnectedMember));
		}
	}

	if (!ActiveHost.Controller)
	{
		OutError = TEXT("Listen server Active Host is not Ready in the recovery roster");
		return false;
	}
	BackupCandidates.Sort([](
		const FConnectedMigrationMember& A,
		const FConnectedMigrationMember& B)
	{
		return A.Member.OriginalSlot < B.Member.OriginalSlot;
	});

	const FGuid MatchId = SnapshotCoordinator->GetOrCreateActiveMatchId();
	uint64 AuthorityEpoch = 0;
	if (!MigrationSubsystem->PrepareAuthorityTerm(
		MatchId,
		ActiveHost.Member,
		RosterRevision,
		AuthorityEpoch,
		OutError))
	{
		return false;
	}

	FWeaponMigrationRoleAssignment Assignment;
	Assignment.MatchId = MatchId;
	Assignment.AuthorityEpoch = AuthorityEpoch;
	Assignment.RosterRevision = RosterRevision;
	Assignment.ActiveHost = ActiveHost.Member;
	if (BackupCandidates.IsValidIndex(0))
	{
		Assignment.BackupA = BackupCandidates[0].Member;
	}
	if (BackupCandidates.IsValidIndex(1))
	{
		Assignment.BackupB = BackupCandidates[1].Member;
	}

	const FWeaponMigrationRoleAssignment* PreviousAssignment =
		MigrationSubsystem->GetCurrentRoleAssignment();
	const bool bAssignmentChanged = MigrationSubsystem->IsCurrentRoleAssignmentProvisional() ||
		!PreviousAssignment ||
		!Assignment.IdentifiesSameAssignment(*PreviousAssignment);
	if (!MigrationSubsystem->SetAuthorityRoleAssignment(Assignment, OutError))
	{
		return false;
	}

	if (bAssignmentChanged)
	{
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			if (AWeaponPlayerController* Controller = Cast<AWeaponPlayerController>(It->Get()))
			{
				Controller->ClientUpdateMigrationRoleAssignment(Assignment);
			}
		}

		const int32 BackupCount = Assignment.BackupA.IsAssigned() + Assignment.BackupB.IsAssigned();
		if (BackupCount < 2)
		{
			UE_LOG(
				LogLetsDeepRock,
				Warning,
				TEXT("Host migration redundancy reduced to %d backup(s) for Match %s Epoch %llu"),
				BackupCount,
				*Assignment.MatchId.ToString(),
				Assignment.AuthorityEpoch);
		}
	}

	if (!Assignment.BackupA.IsAssigned() || !Assignment.BackupB.IsAssigned())
	{
		MigrationSubsystem->StopHostSnapshotTransport();
		return true;
	}

	AWeaponPlayerController* BackupAController = BackupCandidates[0].Controller;
	AWeaponPlayerController* BackupBController = BackupCandidates[1].Controller;
	if (bAssignmentChanged || !MigrationSubsystem->IsHostSnapshotTransportConfigured(
		BackupAController,
		BackupBController,
		AuthorityEpoch))
	{
		return MigrationSubsystem->StartHostSnapshotTransport(
			BackupAController,
			BackupBController,
			AuthorityEpoch,
			OutError);
	}
	return true;
}

bool AWeaponGameMode::EnterGameplayRecoveryBarrier()
{
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!HasAuthority() || !WeaponGameState)
	{
		return false;
	}
	if (WeaponGameState->IsGameplayRecoveryBlocked())
	{
		return true;
	}

	const EWeaponMatchPhase FrozenPhase = WeaponGameState->GetMatchPhase();
	FrozenMatchPhaseRemaining = WeaponGameState->GetRemainingPhaseTime();
	GetWorldTimerManager().ClearTimer(MatchPhaseTimer);
	for (const auto& TimerPair : RespawnTimers)
	{
		GetWorldTimerManager().PauseTimer(TimerPair.Value);
	}
	// 倒地流血计时同语义:屏障内暂停。
	for (const auto& BleedPair : BleedOutTimers)
	{
		GetWorldTimerManager().PauseTimer(BleedPair.Value);
	}
	// 撤离倒计时与复活计时同语义:屏障内暂停,解除后续跑。
	GetWorldTimerManager().PauseTimer(ExtractionTimer);

	WeaponGameState->SetGameplayRecoveryBlockedAuthority(true);
	WeaponGameState->SetMatchPhase(FrozenPhase);
	ApplyGameplayRecoveryBarrierToWorld(true);
	return true;
}

bool AWeaponGameMode::ReleaseGameplayRecoveryBarrier()
{
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!HasAuthority() || !WeaponGameState || !WeaponGameState->IsGameplayRecoveryBlocked())
	{
		return false;
	}

	const EWeaponMatchPhase FrozenPhase = WeaponGameState->GetMatchPhase();
	const float RemainingPhaseTime = FrozenMatchPhaseRemaining;
	ApplyGameplayRecoveryBarrierToWorld(false);
	for (const auto& TimerPair : RespawnTimers)
	{
		// 复活时间戳同理:按暂停剩余时长重新锚定后再续跑。
		if (AWeaponPlayerState* RespawnPS = TimerPair.Key ? TimerPair.Key->GetPlayerState<AWeaponPlayerState>() : nullptr)
		{
			RespawnPS->SetRespawnServerTime(WeaponGameState->GetServerWorldTimeSeconds() +
				GetWorldTimerManager().GetTimerRemaining(TimerPair.Value));
		}
		GetWorldTimerManager().UnPauseTimer(TimerPair.Value);
	}
	for (const auto& BleedPair : BleedOutTimers)
	{
		// 流血结束时间戳同理:按暂停剩余时长重新锚定后再续跑。
		if (AWeaponPlayerState* BleedPS = BleedPair.Key ? BleedPair.Key->GetPlayerState<AWeaponPlayerState>() : nullptr)
		{
			BleedPS->SetDownedServerTime(WeaponGameState->GetServerWorldTimeSeconds() +
				GetWorldTimerManager().GetTimerRemaining(BleedPair.Value));
		}
		GetWorldTimerManager().UnPauseTimer(BleedPair.Value);
	}
	if (GetWorldTimerManager().IsTimerPaused(ExtractionTimer))
	{
		// 复制的是绝对开门时间:按暂停中的剩余时长重新锚定,HUD 倒计时才不会在冻结期间提前走完。
		FWeaponObjectiveState ResumedObjectiveState = WeaponGameState->GetObjectiveState();
		ResumedObjectiveState.ExtractionReadyServerTime = WeaponGameState->GetServerWorldTimeSeconds() +
			GetWorldTimerManager().GetTimerRemaining(ExtractionTimer);
		WeaponGameState->SetObjectiveStateAuthority(ResumedObjectiveState);
		GetWorldTimerManager().UnPauseTimer(ExtractionTimer);
	}
	ArmMatchPhaseTimer(FrozenPhase, RemainingPhaseTime);
	FrozenMatchPhaseRemaining = 0.0f;
	WeaponGameState->SetGameplayRecoveryBlockedAuthority(false);
	FString MigrationError;
	if (!RefreshHostMigrationRoles(MigrationError))
	{
		UE_LOG(LogLetsDeepRock, Error, TEXT("Failed to publish migration roles after recovery: %s"), *MigrationError);
		EnterGameplayRecoveryBarrier();
		return false;
	}

	if (RemainingPhaseTime <= 0.0f)
	{
		if (FrozenPhase == EWeaponMatchPhase::Starting)
		{
			BeginWeaponMatch();
		}
		else if (FrozenPhase == EWeaponMatchPhase::Ending)
		{
			ReturnToWaitingForPlayers();
		}
	}

	// 屏障内的死亡/救起/快照恢复不会触发失败检查;此时屏障标志已清、池与 LifeState 已完整恢复,补做一次。
	CheckMissionFailure();
	return true;
}

void AWeaponGameMode::ApplyRecoveredMatchPhase(
	const EWeaponMatchPhase Phase,
	const float RemainingPhaseTime)
{
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	check(HasAuthority() && WeaponGameState && WeaponGameState->IsGameplayRecoveryBlocked());

	GetWorldTimerManager().ClearTimer(MatchPhaseTimer);
	FrozenMatchPhaseRemaining = FMath::Max(0.0f, RemainingPhaseTime);
	WeaponGameState->SetMatchPhase(Phase);
}

void AWeaponGameMode::ApplyGameplayRecoveryBarrierToWorld(const bool bBlocked)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return;
	}

	for (TActorIterator<AWeaponProjectile> It(World); It; ++It)
	{
		It->SetGameplayRecoveryBlocked(bBlocked);
	}
	for (TActorIterator<AWeaponCharacter> It(World); It; ++It)
	{
		It->SetGameplayRecoveryBlocked(bBlocked);
	}
	for (TActorIterator<ANPCBase> It(World); It; ++It)
	{
		It->SetGameplayRecoveryBlocked(bBlocked);
	}
	for (TActorIterator<ADrone> It(World); It; ++It)
	{
		It->SetGameplayRecoveryBlocked(bBlocked);
	}
	for (TActorIterator<AEnemyAIController> It(World); It; ++It)
	{
		It->SetGameplayRecoveryBlocked(bBlocked);
	}
}

void AWeaponGameMode::ArmMatchPhaseTimer(
	const EWeaponMatchPhase Phase,
	const float RemainingPhaseTime)
{
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!WeaponGameState)
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(MatchPhaseTimer);
	WeaponGameState->SetMatchPhase(Phase, RemainingPhaseTime);
	if (RemainingPhaseTime <= 0.0f)
	{
		return;
	}

	if (Phase == EWeaponMatchPhase::Starting)
	{
		GetWorldTimerManager().SetTimer(
			MatchPhaseTimer,
			this,
			&AWeaponGameMode::BeginWeaponMatch,
			RemainingPhaseTime,
			false);
	}
	else if (Phase == EWeaponMatchPhase::Ending)
	{
		GetWorldTimerManager().SetTimer(
			MatchPhaseTimer,
			this,
			&AWeaponGameMode::ReturnToWaitingForPlayers,
			RemainingPhaseTime,
			false);
	}
}

bool AWeaponGameMode::SetRecoveryRoster(
	const TArray<FWeaponRecoveryRosterSnapshot>& NewRoster,
	const uint64 RestoredRevision)
{
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!HasAuthority() || !WeaponGameState || RestoredRevision == MAX_uint64)
	{
		return false;
	}

	TMap<FUniqueNetIdRepl, int32> ImportedReservations;
	TMap<FUniqueNetIdRepl, FWeaponRecoveryRosterEntry> ImportedRoster;
	TMap<FUniqueNetIdRepl, float> PendingDurations;
	TSet<int32> ReservedSlots;
	TSet<FUniqueNetIdRepl> ConnectedPlayerIds;
	for (const APlayerState* PlayerState : WeaponGameState->PlayerArray)
	{
		if (!PlayerState || !PlayerState->GetUniqueId().IsValid() ||
			ConnectedPlayerIds.Contains(PlayerState->GetUniqueId()))
		{
			return false;
		}
		ConnectedPlayerIds.Add(PlayerState->GetUniqueId());
	}

	for (const FWeaponRecoveryRosterSnapshot& SavedEntry : NewRoster)
	{
		const bool bKnownStatus = SavedEntry.Status == EWeaponRecoveryPlayerStatus::Pending ||
			SavedEntry.Status == EWeaponRecoveryPlayerStatus::Ready ||
			SavedEntry.Status == EWeaponRecoveryPlayerStatus::Departed;
		if (!SavedEntry.PlayerId.IsValid() || ImportedRoster.Contains(SavedEntry.PlayerId) ||
			SavedEntry.OriginalSlot < 0 || SavedEntry.OriginalSlot >= WeaponGameState->GetMaxPlayers() ||
			!bKnownStatus || !FMath::IsFinite(SavedEntry.RemainingGraceSeconds) ||
			SavedEntry.RemainingGraceSeconds < 0.0f ||
			(SavedEntry.Status != EWeaponRecoveryPlayerStatus::Pending &&
			 SavedEntry.RemainingGraceSeconds != 0.0f))
		{
			return false;
		}

		FWeaponRecoveryRosterEntry& Entry = ImportedRoster.Add(SavedEntry.PlayerId);
		Entry.PlayerId = SavedEntry.PlayerId;
		Entry.OriginalSlot = SavedEntry.OriginalSlot;

		if (ConnectedPlayerIds.Contains(SavedEntry.PlayerId))
		{
			if (SavedEntry.Status == EWeaponRecoveryPlayerStatus::Departed)
			{
				return false;
			}
			Entry.Status = EWeaponRecoveryPlayerStatus::Ready;
		}
		else if (SavedEntry.Status == EWeaponRecoveryPlayerStatus::Departed ||
			(SavedEntry.Status == EWeaponRecoveryPlayerStatus::Pending &&
			 SavedEntry.RemainingGraceSeconds <= 0.0f))
		{
			Entry.Status = EWeaponRecoveryPlayerStatus::Departed;
		}
		else if (SavedEntry.Status == EWeaponRecoveryPlayerStatus::Pending ||
			SavedEntry.Status == EWeaponRecoveryPlayerStatus::Ready)
		{
			Entry.Status = EWeaponRecoveryPlayerStatus::Pending;
			PendingDurations.Add(
				SavedEntry.PlayerId,
				SavedEntry.Status == EWeaponRecoveryPlayerStatus::Pending
					? SavedEntry.RemainingGraceSeconds
					: PendingGracePeriodSeconds);
		}
		else
		{
			return false;
		}

		if (Entry.Status != EWeaponRecoveryPlayerStatus::Departed)
		{
			if (ReservedSlots.Contains(Entry.OriginalSlot))
			{
				return false;
			}
			ReservedSlots.Add(Entry.OriginalSlot);
			ImportedReservations.Add(Entry.PlayerId, Entry.OriginalSlot);
		}
	}

	for (const FUniqueNetIdRepl& ConnectedPlayerId : ConnectedPlayerIds)
	{
		if (!ImportedRoster.Contains(ConnectedPlayerId))
		{
			return false;
		}
	}

	for (auto& TimerPair : PendingGraceTimers)
	{
		GetWorldTimerManager().ClearTimer(TimerPair.Value);
	}
	PendingGraceTimers.Reset();

	PlayerSlotReservations = MoveTemp(ImportedReservations);
	RecoveryRoster = MoveTemp(ImportedRoster);
	RosterRevision = FMath::Max(RosterRevision, RestoredRevision) + 1;
	for (const auto& [PlayerId, RemainingGraceSeconds] : PendingDurations)
	{
		SchedulePendingGrace(PlayerId, RemainingGraceSeconds);
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
	SchedulePendingGrace(PlayerId, PendingGracePeriodSeconds);
}

void AWeaponGameMode::SchedulePendingGrace(
	const FUniqueNetIdRepl& PlayerId,
	const float RemainingGraceSeconds)
{
	FWeaponRecoveryRosterEntry* Entry = RecoveryRoster.Find(PlayerId);
	if (!Entry || Entry->Status != EWeaponRecoveryPlayerStatus::Pending)
	{
		return;
	}

	Entry->PendingDeadlineServerTime = GetWorld()->GetTimeSeconds() + RemainingGraceSeconds;
	ClearPendingGraceTimer(PlayerId);
	if (RemainingGraceSeconds <= 0.0f)
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
		RemainingGraceSeconds,
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
	FString MigrationError;
	if (!RefreshHostMigrationRoles(MigrationError))
	{
		UE_LOG(LogLetsDeepRock, Warning, TEXT("Failed to refresh migration roles after departure: %s"), *MigrationError);
	}
	return true;
}

bool AWeaponGameMode::PreserveLeavingPlayerAttribution(AWeaponPlayerController* LeavingController)
{
	if (!HasAuthority() || !LeavingController)
	{
		return false;
	}

	AWeaponCharacter* Pawn = Cast<AWeaponCharacter>(LeavingController->GetPawn());
	AWeaponPlayerState* PlayerState = LeavingController->GetPlayerState<AWeaponPlayerState>();
	const FUniqueNetIdRepl PlayerId = PlayerState ? PlayerState->GetUniqueId() : FUniqueNetIdRepl();
	const TSet<TWeakObjectPtr<AWeaponProjectile>>* ActiveProjectiles =
		ActiveAttributionProjectiles.Find(PlayerId);
	if (!Pawn || !PlayerState || !PlayerId.IsValid() || !ActiveProjectiles)
	{
		return false;
	}

	FWeaponAttributionTombstone Tombstone;
	for (const TWeakObjectPtr<AWeaponProjectile>& WeakProjectile : *ActiveProjectiles)
	{
		AWeaponProjectile* Projectile = WeakProjectile.Get();
		if (Projectile && Projectile->GetInitializationState().InstigatorActor == Pawn)
		{
			Tombstone.Projectiles.Add(Projectile);
		}
	}
	if (Tombstone.Projectiles.IsEmpty())
	{
		return false;
	}

	if (AWeaponGameState* WeaponGameState = GetWorld()->GetGameState<AWeaponGameState>())
	{
		WeaponGameState->RemovePlayerState(PlayerState);
	}
	LeavingController->UnPossess();
	Pawn->InitializeAttributionTombstone(PlayerState);

	Tombstone.Pawn = Pawn;
	Tombstone.PlayerState = PlayerState;
	AttributionTombstones.FindOrAdd(PlayerId).Add(MoveTemp(Tombstone));
	return true;
}

bool AWeaponGameMode::HandleAttributionPlayerStateDeactivated(AWeaponPlayerState* PlayerState)
{
	if (!HasAuthority() || !PlayerState)
	{
		return false;
	}

	const FUniqueNetIdRepl PlayerId = PlayerState->GetUniqueId();
	TArray<FWeaponAttributionTombstone>* Tombstones = AttributionTombstones.Find(PlayerId);
	if (!Tombstones)
	{
		return false;
	}

	bool bFound = false;
	for (FWeaponAttributionTombstone& Tombstone : *Tombstones)
	{
		if (Tombstone.PlayerState == PlayerState)
		{
			Tombstone.bPlayerStateDeactivated = true;
			bFound = true;
			break;
		}
	}
	if (bFound)
	{
		CleanupCompletedAttributionTombstones(PlayerId);
	}
	return bFound;
}

void AWeaponGameMode::RegisterAttributionProjectile(
	const FUniqueNetIdRepl& PlayerId,
	AWeaponProjectile* Projectile)
{
	if (HasAuthority() && PlayerId.IsValid() && Projectile)
	{
		ActiveAttributionProjectiles.FindOrAdd(PlayerId).Add(Projectile);
	}
}

void AWeaponGameMode::UnregisterAttributionProjectile(
	const FUniqueNetIdRepl& PlayerId,
	AWeaponProjectile* Projectile)
{
	if (!HasAuthority() || !PlayerId.IsValid() || !Projectile)
	{
		return;
	}

	if (TSet<TWeakObjectPtr<AWeaponProjectile>>* ActiveProjectiles =
		ActiveAttributionProjectiles.Find(PlayerId))
	{
		ActiveProjectiles->Remove(Projectile);
		if (ActiveProjectiles->IsEmpty())
		{
			ActiveAttributionProjectiles.Remove(PlayerId);
		}
	}

	if (TArray<FWeaponAttributionTombstone>* Tombstones = AttributionTombstones.Find(PlayerId))
	{
		for (FWeaponAttributionTombstone& Tombstone : *Tombstones)
		{
			Tombstone.Projectiles.Remove(Projectile);
		}
	}
	CleanupCompletedAttributionTombstones(PlayerId);
}

void AWeaponGameMode::AdoptRecoveredAttributionTombstone(
	const FUniqueNetIdRepl& PlayerId,
	AWeaponCharacter* Pawn,
	AWeaponPlayerState* PlayerState)
{
	check(HasAuthority() && PlayerId.IsValid() && Pawn && PlayerState);

	FWeaponAttributionTombstone Tombstone;
	if (const TSet<TWeakObjectPtr<AWeaponProjectile>>* ActiveProjectiles =
		ActiveAttributionProjectiles.Find(PlayerId))
	{
		for (const TWeakObjectPtr<AWeaponProjectile>& WeakProjectile : *ActiveProjectiles)
		{
			AWeaponProjectile* Projectile = WeakProjectile.Get();
			if (Projectile && Projectile->GetInitializationState().InstigatorActor == Pawn)
			{
				Tombstone.Projectiles.Add(Projectile);
			}
		}
	}
	check(!Tombstone.Projectiles.IsEmpty());

	if (AWeaponGameState* WeaponGameState = GetWorld()->GetGameState<AWeaponGameState>())
	{
		WeaponGameState->RemovePlayerState(PlayerState);
	}
	Tombstone.Pawn = Pawn;
	Tombstone.PlayerState = PlayerState;
	Tombstone.bPlayerStateDeactivated = true;
	AttributionTombstones.FindOrAdd(PlayerId).Add(MoveTemp(Tombstone));
}

void AWeaponGameMode::GetAttributionTombstoneActors(
	TMap<FUniqueNetIdRepl, AWeaponCharacter*>& OutPawns,
	TMap<FUniqueNetIdRepl, AWeaponPlayerState*>& OutPlayerStates) const
{
	OutPawns.Reset();
	OutPlayerStates.Reset();
	for (const auto& [PlayerId, Tombstones] : AttributionTombstones)
	{
		for (const FWeaponAttributionTombstone& Tombstone : Tombstones)
		{
			AWeaponCharacter* Pawn = Tombstone.Pawn.Get();
			AWeaponPlayerState* PlayerState = Tombstone.PlayerState.Get();
			if (Pawn && PlayerState && !Tombstone.Projectiles.IsEmpty())
			{
				OutPawns.Add(PlayerId, Pawn);
				OutPlayerStates.Add(PlayerId, PlayerState);
				break;
			}
		}
	}
}

void AWeaponGameMode::CleanupCompletedAttributionTombstones(const FUniqueNetIdRepl& PlayerId)
{
	TArray<FWeaponAttributionTombstone>* Tombstones = AttributionTombstones.Find(PlayerId);
	if (!Tombstones)
	{
		return;
	}

	for (int32 Index = Tombstones->Num() - 1; Index >= 0; --Index)
	{
		FWeaponAttributionTombstone& Tombstone = (*Tombstones)[Index];
		for (auto It = Tombstone.Projectiles.CreateIterator(); It; ++It)
		{
			if (!It->IsValid())
			{
				It.RemoveCurrent();
			}
		}
		if (!Tombstone.bPlayerStateDeactivated || !Tombstone.Projectiles.IsEmpty())
		{
			continue;
		}

		if (AWeaponCharacter* Pawn = Tombstone.Pawn.Get())
		{
			Pawn->SetPlayerState(nullptr);
			Pawn->Destroy();
		}
		if (AWeaponPlayerState* PlayerState = Tombstone.PlayerState.Get())
		{
			PlayerState->Destroy();
		}
		Tombstones->RemoveAtSwap(Index);
	}
	if (Tombstones->IsEmpty())
	{
		AttributionTombstones.Remove(PlayerId);
	}
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

void AWeaponGameMode::OnPostLogin(AController* NewPlayer)
{
	APlayerController* NewPlayerController = Cast<APlayerController>(NewPlayer);
	if (!NewPlayerController)
	{
		Super::OnPostLogin(NewPlayer);
		return;
	}
	
	AWeaponPlayerState* WeaponPlayerState = NewPlayerController->GetPlayerState<AWeaponPlayerState>();
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;

	if (!WeaponPlayerState || !WeaponGameState)
	{
		UE_LOG(LogTemp, Error, TEXT("Failed to initialize player lobby state for %s."),
			*GetNameSafe(NewPlayerController));
		Super::OnPostLogin(NewPlayer);
		return;
	}
	
	const FUniqueNetIdRepl& PlayerId = WeaponPlayerState->GetUniqueId();
	const int32 AvailableSlot = FindOrReservePlayerSlot(PlayerId);
	
	// 理论上 PreLogin 已阻止此情况；保留保护，避免异常连接污染状态。
	if (AvailableSlot == INDEX_NONE)
	{
		UE_LOG(LogTemp, Warning, TEXT("No available player slot for %s (%s)."),
			*GetNameSafe(NewPlayerController), *PlayerId.ToDebugString());
		Super::OnPostLogin(NewPlayer);
		return;
	}
	
	WeaponPlayerState->SetPlayerSlotIndex(AvailableSlot);
	// 大厅已确认 ready 后才 ServerTravel；重连世界的 PlayerState 是新建的，ready 不会带过来。
	// 任务图内不再要求二次 ready，否则没人能开局（无刷怪、战斗 HUD 不显示）。
	WeaponPlayerState->SetPlayerReady(bArrivedFromLobby);
	MarkPlayerReady(PlayerId, AvailableSlot);
	
	RefreshMatchPhase();
	FString MigrationError;
	if (!RefreshHostMigrationRoles(MigrationError))
	{
		UE_LOG(LogLetsDeepRock, Warning, TEXT("Failed to refresh migration roles after login: %s"), *MigrationError);
	}

	// 单人（Standalone）：没有"等待队友准备"的社交语义。直接视为已准备并尝试开局，
	// 避免单人模式仍走多人大厅流程（此前表现为小队 1/4 + 等待矿工接入）。
	if (GetNetMode() == NM_Standalone)
	{
		WeaponPlayerState->SetPlayerReady(true);
		TryStartWeaponMatch();
	}
	else if (bArrivedFromLobby)
	{
		TryStartLobbyArrivedMatch();
	}

	// AGameModeBase calls HandleStartingNewPlayer immediately after this virtual hook returns.
	// The slot must therefore be installed before forwarding the Blueprint OnPostLogin event.
	Super::OnPostLogin(NewPlayer);
}

void AWeaponGameMode::RestartPlayer(AController* NewPlayer)
{
	if (!NewPlayer || NewPlayer->IsPendingKillPending())
	{
		return;
	}

	if (AActor* StartSpot = ChoosePlayerStart(NewPlayer))
	{
		RestartPlayerAtPlayerStart(NewPlayer, StartSpot);
	}
	else
	{
		const AWeaponPlayerState* WeaponPlayerState = NewPlayer->GetPlayerState<AWeaponPlayerState>();
		const int32 PlayerSlot = WeaponPlayerState ? WeaponPlayerState->GetPlayerSlotIndex() : INDEX_NONE;
		const FVector SpawnLocation = ComputeFallbackSpawnLocation(NewPlayer, PlayerSlot);
		const FTransform SpawnTransform(FRotator::ZeroRotator, SpawnLocation);

		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("No usable PlayerStart for %s; using Slot %d fallback at %s"),
			*GetNameSafe(NewPlayer),
			PlayerSlot,
			*SpawnLocation.ToCompactString());
		RestartPlayerAtTransform(NewPlayer, SpawnTransform);
	}

	if (AWeaponPlayerController* WeaponPlayerController = Cast<AWeaponPlayerController>(NewPlayer))
	{
		if (UGameInstance* GI = GetGameInstance())
		{
			if (UWeaponSnapshotCoordinator* Coordinator = GI->GetSubsystem<UWeaponSnapshotCoordinator>())
			{
				FString RestoreError;
				if (!Coordinator->RestoreReconnectedPlayer(WeaponPlayerController, RestoreError) &&
					AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
				{
					UE_LOG(LogLetsDeepRock, Warning, TEXT("Reconnect Snapshot apply skipped for %s: %s"),
						*GetNameSafe(WeaponPlayerController), *RestoreError);
				}
			}
		}

		// 普通掉线重连(无迁移屏障/快照):按 Logout 时的记录还原死亡状态,不让重进变成免费复活。
		ApplyDepartedLifeState(NewPlayer);
	}
}

FVector AWeaponGameMode::ComputeFallbackSpawnLocation(AController* NewPlayer, const int32 PlayerSlot)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		return FVector::ZeroVector;
	}

	// 锚点：GridMap 的 PlayerSpawn 标记（随种子生成、保证在可走地面上）；没有则用任一 PlayerStart；都没有才用原点。
	// 旧实现固定以世界原点为中心，原点附近不一定有地板（L_GridMapStage4Test 的 Slot 2 会掉出地图）。
	FVector Anchor = FVector::ZeroVector;
	bool bHasAnchor = false;
	for (TActorIterator<AGridMapManager> It(World); It && !bHasAnchor; ++It)
	{
		TArray<FVector> SpawnMarkers;
		if (It->GetMarkerWorldLocations(EGridMapMarkerType::PlayerSpawn, SpawnMarkers) && !SpawnMarkers.IsEmpty())
		{
			Anchor = SpawnMarkers[0];
			bHasAnchor = true;
		}
	}
	for (TActorIterator<APlayerStart> It(World); It && !bHasAnchor; ++It)
	{
		Anchor = It->GetActorLocation();
		bHasAnchor = true;
	}

	FVector2D Direction(-1.0f, -1.0f);
	switch (PlayerSlot)
	{
	case 1:
		Direction = FVector2D(1.0f, -1.0f);
		break;
	case 2:
		Direction = FVector2D(-1.0f, 1.0f);
		break;
	case 3:
		Direction = FVector2D(1.0f, 1.0f);
		break;
	default:
		break;
	}

	float HalfHeight = 88.0f;
	if (const UClass* PawnClass = GetDefaultPawnClassForController(NewPlayer))
	{
		if (const ACharacter* CharacterCDO = Cast<ACharacter>(PawnClass->GetDefaultObject()))
		{
			HalfHeight = CharacterCDO->GetCapsuleComponent()->GetScaledCapsuleHalfHeight();
		}
	}

	// 先试 Slot 偏移点，再退回锚点本身；向下找 WorldStatic 地面，胶囊底部贴地。
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(WeaponFallbackSpawn), false);
	const FVector Candidates[] = {
		Anchor + FVector(Direction.X * WeaponFallbackSpawnSpacing, Direction.Y * WeaponFallbackSpawnSpacing, 0.0f),
		Anchor,
	};
	for (const FVector& Candidate : Candidates)
	{
		FHitResult GroundHit;
		if (World->LineTraceSingleByObjectType(GroundHit, Candidate + FVector(0.0f, 0.0f, 200.0f),
			Candidate - FVector(0.0f, 0.0f, 2000.0f), FCollisionObjectQueryParams(ECC_WorldStatic), QueryParams))
		{
			return GroundHit.ImpactPoint + FVector(0.0f, 0.0f, HalfHeight + 5.0f);
		}
	}
	return Anchor + FVector(0.0f, 0.0f, HalfHeight + 5.0f);
}

APawn* AWeaponGameMode::SpawnDefaultPawnAtTransform_Implementation(
	AController* NewPlayer,
	const FTransform& SpawnTransform)
{
	UClass* PawnClass = GetDefaultPawnClassForController(NewPlayer);
	if (!PawnClass || !GetWorld())
	{
		return nullptr;
	}

	FActorSpawnParameters SpawnInfo;
	SpawnInfo.Instigator = GetInstigator();
	SpawnInfo.ObjectFlags |= RF_Transient;
	SpawnInfo.SpawnCollisionHandlingOverride =
		ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;
	APawn* ResultPawn = GetWorld()->SpawnActor<APawn>(PawnClass, SpawnTransform, SpawnInfo);
	if (!ResultPawn)
	{
		UE_LOG(
			LogLetsDeepRock,
			Error,
			TEXT("Failed to spawn Pawn %s for %s at %s"),
			*GetNameSafe(PawnClass),
			*GetNameSafe(NewPlayer),
			*SpawnTransform.ToHumanReadableString());
	}
	return ResultPawn;
}

void AWeaponGameMode::Logout(AController* ExitingController)
{
	FUniqueNetIdRepl ExitingPlayerId;
	APlayerState* ExitingPlayerState = nullptr;
	if (ExitingController)
	{
		if (APlayerState* PlayerState = ExitingController->GetPlayerState<APlayerState>())
		{
			ExitingPlayerState = PlayerState;
			ExitingPlayerId = PlayerState->GetUniqueId();
		}
	}

	// 先记录生存状态(清计时器不改 LifeState,但要在 PlayerState 被移除前读取身份)
	RecordDepartedLifeState(ExitingController);

	// 首先清理 Timer，防止Super::Logout 后，Controller/PlayerState 已被移除或销毁，不适合作为 Timer Map 的查找键
	if (ExitingController)
	{
		ClearPlayerLifeTimers(ExitingController);
	}
	if (ExitingPlayerId.IsValid())
	{
		MarkPlayerPending(ExitingPlayerId);
	}
	if (ExitingPlayerState)
	{
		if (AWeaponGameState* WeaponGameState = GetWorld()->GetGameState<AWeaponGameState>())
		{
			WeaponGameState->RemovePlayerState(ExitingPlayerState);
		}
	}
	
	Super::Logout(ExitingController);
	
	RefreshMatchPhase();
	FString MigrationError;
	if (!RefreshHostMigrationRoles(MigrationError))
	{
		UE_LOG(LogLetsDeepRock, Warning, TEXT("Failed to refresh migration roles after logout: %s"), *MigrationError);
	}

	// 离线者此时已从 PlayerArray 移除(连接数/存活数都不再计入);池空时剩余存活者掉线 = 全员倒下。
	CheckMissionFailure();
}

void AWeaponGameMode::PlayerDied(AController* DeadPlayerController)
{
	if (!HasAuthority() || !DeadPlayerController ||
		AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}
	
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (WeaponGameState && WeaponGameState->GetMatchPhase() == EWeaponMatchPhase::InGame)
	{
		// 局内:增援池有余量 → 延迟自动增援;池空则不排复活(HUD 时间戳清零),并检查任务失败。
		if (WeaponGameState->GetReinforcementPool() > 0)
		{
			ArmRespawnTimer(DeadPlayerController, AutoReinforceDelay);
		}
		else if (AWeaponPlayerState* PS = DeadPlayerController->GetPlayerState<AWeaponPlayerState>())
		{
			PS->SetRespawnServerTime(0.0f);
		}
		CheckMissionFailure();
		return;
	}

	ArmRespawnTimer(DeadPlayerController, RespawnDelay);
}

void AWeaponGameMode::PlayerHealthDepleted(AController* PlayerController)
{
	if (!HasAuthority() || !PlayerController)
	{
		return;
	}

	AWeaponCharacter* Character = Cast<AWeaponCharacter>(PlayerController->GetPawn());
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	AWeaponPlayerState* PS = PlayerController->GetPlayerState<AWeaponPlayerState>();

	// 仅局内(且屏障未关)才有倒地规则;其余阶段保持旧行为:死亡 → RespawnDelay → 免费复活。
	const bool bInGame = WeaponGameState && WeaponGameState->GetMatchPhase() == EWeaponMatchPhase::InGame &&
		!WeaponGameState->IsGameplayRecoveryBlocked();
	if (bInGame && Character && PS && PS->GetLifeState() == EWeaponPlayerLifeState::Up &&
		WeaponShouldEnterDowned(WeaponCountUpPlayers(GetWorld(), PlayerController), bDownedRequiresReviver))
	{
		Character->EnterDowned();
		PS->SetLifeStateAuthority(EWeaponPlayerLifeState::Downed);
		ArmBleedOutTimer(PlayerController, BleedOutSeconds);
		UE_LOG(LogLetsDeepRock, Log, TEXT("[Reinforce] %s downed, bleed-out %.1f s"),
			*GetNameSafe(PlayerController), BleedOutSeconds);
		CheckMissionFailure();
		return;
	}

	if (Character)
	{
		Character->EnterDeath();
	}
	PlayerDied(PlayerController);
}

void AWeaponGameMode::PlayerDownedDamaged(AController* PlayerController, const float Damage)
{
	if (!HasAuthority() || !PlayerController || Damage <= 0.0f ||
		AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}

	const AWeaponPlayerState* PS = PlayerController->GetPlayerState<AWeaponPlayerState>();
	const FTimerHandle* BleedTimer = BleedOutTimers.Find(PlayerController);
	if (!PS || PS->GetLifeState() != EWeaponPlayerLifeState::Downed || !BleedTimer)
	{
		return;
	}

	const UWeaponHealthSet* HealthSet = PS->GetHealthSet();
	const float Remaining = GetWorldTimerManager().GetTimerRemaining(*BleedTimer) -
		WeaponDownedDamageToSeconds(Damage, HealthSet ? HealthSet->GetMaxHealth() : 0.0f, BleedOutSeconds);
	if (Remaining <= 0.0f)
	{
		ExpireBleedOut(PlayerController);
	}
	else
	{
		ArmBleedOutTimer(PlayerController, Remaining);
	}
}

void AWeaponGameMode::RestoreDownedPlayer(AController* PlayerController, const float RemainingSeconds)
{
	AWeaponPlayerState* PS = PlayerController ? PlayerController->GetPlayerState<AWeaponPlayerState>() : nullptr;
	if (!HasAuthority() || !PS)
	{
		return;
	}

	if (AWeaponCharacter* Character = Cast<AWeaponCharacter>(PlayerController->GetPawn()))
	{
		Character->EnterDowned();
	}
	PS->SetLifeStateAuthority(EWeaponPlayerLifeState::Downed);
	ArmBleedOutTimer(PlayerController, FMath::Max(RemainingSeconds, 0.1f));
	if (AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		if (FTimerHandle* BleedTimer = BleedOutTimers.Find(PlayerController))
		{
			GetWorldTimerManager().PauseTimer(*BleedTimer);
		}
	}
}

void AWeaponGameMode::ArmBleedOutTimer(AController* DownedController, const float Seconds)
{
	FTimerHandle& BleedTimer = BleedOutTimers.FindOrAdd(DownedController);
	GetWorldTimerManager().ClearTimer(BleedTimer);

	TWeakObjectPtr<AController> WeakController(DownedController);
	GetWorldTimerManager().SetTimer(
		BleedTimer, [this, WeakController]
		{
			if (!WeakController.IsValid())
			{
				return;
			}
			ExpireBleedOut(WeakController.Get());
		},
		Seconds,
		false);

	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (AWeaponPlayerState* PS = DownedController->GetPlayerState<AWeaponPlayerState>(); PS && WeaponGameState)
	{
		PS->SetDownedServerTime(WeaponGameState->GetServerWorldTimeSeconds() + Seconds);
	}
}

void AWeaponGameMode::ExpireBleedOut(AController* DownedController)
{
	if (!HasAuthority() || !DownedController || AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}

	AWeaponPlayerState* PS = DownedController->GetPlayerState<AWeaponPlayerState>();
	if (!PS || PS->GetLifeState() != EWeaponPlayerLifeState::Downed)
	{
		ClearBleedOut(DownedController);
		return;
	}

	ClearBleedOut(DownedController);
	if (AWeaponCharacter* Character = Cast<AWeaponCharacter>(DownedController->GetPawn()))
	{
		Character->EnterDeath();
	}
	PS->SetLifeStateAuthority(EWeaponPlayerLifeState::Dead);
	UE_LOG(LogLetsDeepRock, Log, TEXT("[Reinforce] %s bled out"), *GetNameSafe(DownedController));
	PlayerDied(DownedController);
}

void AWeaponGameMode::RevivePlayer(AController* DownedController)
{
	if (!HasAuthority() || !DownedController || AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}
	AWeaponPlayerState* PS = DownedController->GetPlayerState<AWeaponPlayerState>();
	if (!PS || PS->GetLifeState() != EWeaponPlayerLifeState::Downed)
	{
		return;
	}

	ClearBleedOut(DownedController);
	if (AWeaponCharacter* Character = Cast<AWeaponCharacter>(DownedController->GetPawn()))
	{
		Character->ExitDowned();
	}
	if (UWeaponHealthSet* HealthSet = PS->GetHealthSet())
	{
		HealthSet->SetHealth(HealthSet->GetMaxHealth() * RevivedHealthFraction);
		HealthSet->SetShield(0.0f);
	}
	PS->SetLifeStateAuthority(EWeaponPlayerLifeState::Up);
	UE_LOG(LogLetsDeepRock, Log, TEXT("[Reinforce] %s revived"), *GetNameSafe(DownedController));
}

void AWeaponGameMode::ClearBleedOut(AController* PlayerController)
{
	if (!PlayerController)
	{
		return;
	}
	if (FTimerHandle* BleedTimer = BleedOutTimers.Find(PlayerController))
	{
		GetWorldTimerManager().ClearTimer(*BleedTimer);
		BleedOutTimers.Remove(PlayerController);
	}
	if (AWeaponPlayerState* PS = PlayerController->GetPlayerState<AWeaponPlayerState>())
	{
		PS->SetDownedServerTime(0.0f);
	}
}

void AWeaponGameMode::ClearPlayerLifeTimers(AController* PlayerController)
{
	if (!PlayerController)
	{
		return;
	}
	if (FTimerHandle* RespawnTimer = RespawnTimers.Find(PlayerController))
	{
		GetWorldTimerManager().ClearTimer(*RespawnTimer);
		RespawnTimers.Remove(PlayerController);
	}
	ClearBleedOut(PlayerController);
}

void AWeaponGameMode::RecordDepartedLifeState(AController* ExitingController)
{
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	const AWeaponPlayerState* PS = ExitingController ? ExitingController->GetPlayerState<AWeaponPlayerState>() : nullptr;
	if (!HasAuthority() || !PS || !PS->GetUniqueId().IsValid())
	{
		return;
	}

	const FUniqueNetIdRepl PlayerId = PS->GetUniqueId();
	// 只在局内且屏障未关时记录(迁移有自己的快照路径);其余情况清掉旧记录。
	if (!WeaponGameState || WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::InGame ||
		WeaponGameState->IsGameplayRecoveryBlocked() || PS->GetLifeState() == EWeaponPlayerLifeState::Up)
	{
		DepartedLifeStates.Remove(PlayerId);
		return;
	}

	// 倒地者离线按死亡记(流血计时随 Logout 清掉,不能借掉线免死)。
	DepartedLifeStates.Add(PlayerId, EWeaponPlayerLifeState::Dead);
	UE_LOG(LogLetsDeepRock, Log, TEXT("[Reinforce] %s left while out of action, recorded as Dead"),
		*GetNameSafe(ExitingController));
}

bool AWeaponGameMode::ApplyDepartedLifeState(AController* NewPlayer)
{
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	AWeaponPlayerState* PS = NewPlayer ? NewPlayer->GetPlayerState<AWeaponPlayerState>() : nullptr;
	if (!HasAuthority() || !PS || !WeaponGameState || !PS->GetUniqueId().IsValid() ||
		WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::InGame ||
		WeaponGameState->IsGameplayRecoveryBlocked())
	{
		return false;
	}

	EWeaponPlayerLifeState Recorded = EWeaponPlayerLifeState::Up;
	if (!DepartedLifeStates.RemoveAndCopyValue(PS->GetUniqueId(), Recorded) ||
		Recorded != EWeaponPlayerLifeState::Dead)
	{
		return false;
	}

	// RestartPlayer 已给了活 Pawn:让它死亡,并按与局内死亡相同的规则排增援 / 检查失败。
	if (AWeaponCharacter* Character = Cast<AWeaponCharacter>(NewPlayer->GetPawn()))
	{
		Character->EnterDeath();
	}
	PS->SetLifeStateAuthority(EWeaponPlayerLifeState::Dead);
	if (WeaponGameState->GetReinforcementPool() > 0)
	{
		ArmRespawnTimer(NewPlayer, AutoReinforceDelay);
	}
	else
	{
		PS->SetRespawnServerTime(0.0f);
	}
	UE_LOG(LogLetsDeepRock, Log, TEXT("[Reinforce] %s rejoined, restored as Dead"), *GetNameSafe(NewPlayer));
	CheckMissionFailure();
	return true;
}

bool AWeaponGameMode::TryReinforcePlayer(AController* PlayerController)
{
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!HasAuthority() || !PlayerController || !WeaponGameState ||
		AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return false;
	}

	// 已存活且有 Pawn 的玩家不能被增援(否则白扣一条命):Up 且 Pawn 有效 → 拒绝。
	if (WeaponGetLifeState(PlayerController->GetPlayerState<APlayerState>()) == EWeaponPlayerLifeState::Up &&
		PlayerController->GetPawn())
	{
		return false;
	}

	if (WeaponGameState->GetMatchPhase() == EWeaponMatchPhase::InGame)
	{
		const int32 Pool = WeaponGameState->GetReinforcementPool();
		if (Pool <= 0)
		{
			if (AWeaponPlayerState* PS = PlayerController->GetPlayerState<AWeaponPlayerState>())
			{
				PS->SetRespawnServerTime(0.0f);
			}
			CheckMissionFailure();
			return false;
		}
		WeaponGameState->SetReinforcementPoolAuthority(Pool - 1);
		UE_LOG(LogLetsDeepRock, Log, TEXT("[Reinforce] %s reinforced, pool %d -> %d"),
			*GetNameSafe(PlayerController), Pool, Pool - 1);
	}

	RespawnPlayer(PlayerController);
	return true;
}

void AWeaponGameMode::CheckMissionFailure()
{
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!HasAuthority() || !WeaponGameState || WeaponGameState->IsGameplayRecoveryBlocked() ||
		WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::InGame)
	{
		return;
	}

	const int32 UpPlayers = WeaponCountUpPlayers(GetWorld());
	if (WeaponShouldFailMission(WeaponGameState->GetReinforcementPool(), UpPlayers,
		WeaponGameState->GetConnectedPlayerCount()))
	{
		UE_LOG(LogLetsDeepRock, Log, TEXT("[Reinforce] mission failed: pool empty and no player is up"));
		EndWeaponMatch(EWeaponMissionOutcome::Failed);
	}
}

void AWeaponGameMode::ArmRespawnTimer(AController* PlayerToRespawn, const float DelaySeconds)
{
	FTimerHandle& RespawnTimer = RespawnTimers.FindOrAdd(PlayerToRespawn);
	
	// 同一玩家的重复死亡通知不会创建第二个重生 Timer。
	GetWorldTimerManager().ClearTimer(RespawnTimer);
	
	TWeakObjectPtr<AController> WeakController(PlayerToRespawn);
	
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

			TryReinforcePlayer(PlayerController);
		},
		DelaySeconds,
		false); // 不循环

	// A3:复活倒计时以绝对时间戳复制给 HUD。
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (AWeaponPlayerState* PS = PlayerToRespawn->GetPlayerState<AWeaponPlayerState>(); PS && WeaponGameState)
	{
		PS->SetRespawnServerTime(WeaponGameState->GetServerWorldTimeSeconds() + DelaySeconds);
	}
}

bool AWeaponGameMode::CanStartWeaponMatch() const
{
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!HasAuthority() || !WeaponGameState || WeaponGameState->IsGameplayRecoveryBlocked() ||
		WeaponGameState->GetConnectedPlayerCount() < MinPlayersToStart)
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

void AWeaponGameMode::TryStartLobbyArrivedMatch()
{
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!WeaponGameState || WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::Waiting)
	{
		return;
	}

	// 大厅成员全部到齐立即开局；有人加载慢时等宽限期，过期后已到的人先开局（掉队者按中途加入处理）。
	if (WeaponGameState->GetConnectedPlayerCount() >= ExpectedLobbyPlayers)
	{
		GetWorldTimerManager().ClearTimer(LobbyArrivalTimer);
		TryStartWeaponMatch();
	}
	else if (!GetWorldTimerManager().IsTimerActive(LobbyArrivalTimer))
	{
		GetWorldTimerManager().SetTimer(LobbyArrivalTimer,
			FTimerDelegate::CreateWeakLambda(this, [this]() { TryStartWeaponMatch(); }),
			LobbyArrivalGraceSeconds, false);
	}
}

bool AWeaponGameMode::TryStartLobbyTravel()
{
	UWorld* World = GetWorld();
	const AWeaponGameState* WeaponGameState = World ? World->GetGameState<AWeaponGameState>() : nullptr;
	if (!HasAuthority() || !World || World->GetNetMode() != NM_ListenServer || !WeaponGameState ||
		bLobbyTravelRequested || WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::Waiting ||
		WeaponGameState->GetConnectedPlayerCount() < MinPlayersToStart || !CanStartWeaponMatch())
	{
		return false;
	}

	bLobbyTravelRequested = true;
	// A0：任务设置随 travel options 带入 GridMap（房主在大厅任务终端设置）。
	const FWeaponMissionSettings& Mission = WeaponGameState->GetMissionSettings();
	// FromLobby/LobbyPlayers：任务图据此自动 ready，并在大厅成员到齐后开局。
	const FString TravelURL = FString::Printf(
		TEXT("/Game/GridMap/Maps/L_GridMapStage4Test?listen?Seed=%d?Difficulty=%d?FromLobby?LobbyPlayers=%d"),
		Mission.Seed, Mission.Difficulty, WeaponGameState->GetConnectedPlayerCount());
	if (World->ServerTravel(TravelURL))
	{
		return true;
	}

	bLobbyTravelRequested = false;
	UE_LOG(LogLetsDeepRock, Error, TEXT("Lobby ServerTravel to GridMap Stage 4 failed."));
	return false;
}

void AWeaponGameMode::BeginWeaponMatch()
{
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!WeaponGameState || WeaponGameState->IsGameplayRecoveryBlocked() ||
		WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::Starting)
	{
		return;
	}

	if (!CanStartWeaponMatch())
	{
		WeaponGameState->SetMatchPhase(EWeaponMatchPhase::Waiting);
		return;
	}

	// 上一局的离线生存记录不带入新局(须在下面的 RespawnPlayer→RestartPlayer 之前清掉)。
	DepartedLifeStates.Empty();

	// 增援:池按难度初始化,任务结果清空,全员回到 Up。
	WeaponGameState->SetReinforcementPoolAuthority(WeaponReinforcementPoolForDifficulty(
		WeaponGameState->GetMissionSettings().Difficulty, ReinforcementPoolByDifficulty));
	WeaponGameState->SetMissionOutcomeAuthority(EWeaponMissionOutcome::None);
	for (APlayerState* PlayerState : WeaponGameState->PlayerArray)
	{
		AWeaponPlayerState* WeaponPlayerState = Cast<AWeaponPlayerState>(PlayerState);
		if (WeaponPlayerState && WeaponPlayerState->GetLifeState() != EWeaponPlayerLifeState::Up)
		{
			// 兜底:结算期复活计时未走完就开局时,直接复活(RespawnPlayer 负责置 Up),不能让尸体变 Up。
			if (AController* StragglerController = WeaponPlayerState->GetOwningController())
			{
				if (FTimerHandle* StragglerTimer = RespawnTimers.Find(StragglerController))
				{
					GetWorldTimerManager().ClearTimer(*StragglerTimer);
					RespawnTimers.Remove(StragglerController);
				}
				RespawnPlayer(StragglerController);
			}
		}
	}

	SpawnObjectiveActors();

	// A2:武装波次导演(Combat 房触发刷怪)。
	if (WaveDirector)
	{
		AGridMapManager* WaveManager = nullptr;
		for (TActorIterator<AGridMapManager> ManagerIt(GetWorld()); ManagerIt; ++ManagerIt)
		{
			WaveManager = *ManagerIt;
			break;
		}
		// A2b:难度来自任务设置(InitGameState 已写入 GameState,早于任何开局)。
		WaveDirector->SetDifficulty(WeaponGameState->GetMissionSettings().Difficulty);
		WaveDirector->OnMatchStarted(WaveManager);
	}

	WeaponGameState->SetMatchPhase(EWeaponMatchPhase::InGame);
}

void AWeaponGameMode::SpawnObjectiveActors(const bool bForRecovery)
{
	UWorld* World = GetWorld();
	AWeaponGameState* WeaponGameState = World ? World->GetGameState<AWeaponGameState>() : nullptr;
	if (!World || !HasAuthority() || !WeaponGameState || ObjectiveActor || ExtractionActor ||
		(WeaponGameState->IsGameplayRecoveryBlocked() && !bForRecovery))
	{
		return;
	}

	AGridMapManager* Manager = nullptr;
	for (TActorIterator<AGridMapManager> It(World); It; ++It)
	{
		Manager = *It;
		break;
	}
	if (!Manager)
	{
		UE_LOG(LogLetsDeepRock, Warning, TEXT("[A1] No GridMapManager; objective actors not spawned"));
		return;
	}

	TArray<FVector> ObjectiveLocations;
	Manager->GetMarkerWorldLocations(EGridMapMarkerType::Objective, ObjectiveLocations);
	TArray<FVector> SpawnLocations;
	Manager->GetMarkerWorldLocations(EGridMapMarkerType::PlayerSpawn, SpawnLocations);
	if (ObjectiveLocations.IsEmpty() || SpawnLocations.IsEmpty())
	{
		UE_LOG(LogLetsDeepRock, Warning,
			TEXT("[A1] Recipe missing markers (Objective=%d PlayerSpawn=%d); objective actors not spawned"),
			ObjectiveLocations.Num(), SpawnLocations.Num());
		return;
	}

	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	FVector Location = ObjectiveLocations[0];
	Location.Z += 100.0f;
	ObjectiveActor = World->SpawnActor<AGridMapObjectiveActor>(
		ObjectiveActorClass, FTransform(FRotator::ZeroRotator, Location), Params);

	Location = SpawnLocations[0];
	Location.Z += 10.0f;
	ExtractionActor = World->SpawnActor<AGridMapExtractionActor>(
		ExtractionActorClass, FTransform(FRotator::ZeroRotator, Location), Params);

	UE_LOG(LogLetsDeepRock, Log,
		TEXT("[A1] Objective spawned at %s; extraction zone staged at %s"),
		*ObjectiveLocations[0].ToCompactString(), *SpawnLocations[0].ToCompactString());
}

void AWeaponGameMode::CleanupObjectiveActors()
{
	GetWorldTimerManager().ClearTimer(ExtractionTimer);
	if (ObjectiveActor)
	{
		ObjectiveActor->Destroy();
		ObjectiveActor = nullptr;
	}
	if (ExtractionActor)
	{
		ExtractionActor->Destroy();
		ExtractionActor = nullptr;
	}
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (WeaponGameState)
	{
		WeaponGameState->SetObjectiveStateAuthority(FWeaponObjectiveState());
	}
}

void AWeaponGameMode::HandleObjectiveCompleted()
{
	UWorld* World = GetWorld();
	AWeaponGameState* WeaponGameState = World ? World->GetGameState<AWeaponGameState>() : nullptr;
	if (!World || !HasAuthority() || !WeaponGameState ||
		WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::InGame ||
		WeaponGameState->IsGameplayRecoveryBlocked())
	{
		return;
	}

	FWeaponObjectiveState NewState = WeaponGameState->GetObjectiveState();
	if (NewState.bObjectiveComplete)
	{
		return;
	}
	NewState.bObjectiveComplete = true;
	NewState.ExtractionReadyServerTime = World->GetTimeSeconds() + ExtractionCountdownSeconds;
	WeaponGameState->SetObjectiveStateAuthority(NewState);

	GetWorldTimerManager().SetTimer(
		ExtractionTimer,
		this,
		&AWeaponGameMode::OpenExtraction,
		FMath::Max(ExtractionCountdownSeconds, 0.1f),
		false);
	UE_LOG(LogLetsDeepRock, Log, TEXT("[A1] Objective complete; extraction opens in %.1fs"), ExtractionCountdownSeconds);

	// A2b:撤离倒计时期间,在撤离区附近压一波防御怪。
	if (WaveDirector && ExtractionActor)
	{
		WaveDirector->StartDefenseWave(ExtractionActor->GetActorLocation());
	}
}

void AWeaponGameMode::OpenExtraction()
{
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!WeaponGameState || WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::InGame)
	{
		return;
	}

	FWeaponObjectiveState NewState = WeaponGameState->GetObjectiveState();
	NewState.bExtractionOpen = true;
	WeaponGameState->SetObjectiveStateAuthority(NewState);

	if (ExtractionActor)
	{
		ExtractionActor->SetActive();
	}
	UE_LOG(LogLetsDeepRock, Log, TEXT("[A1] Extraction zone open"));
}

void AWeaponGameMode::EndWeaponMatch(const EWeaponMissionOutcome Outcome)
{
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!HasAuthority() || !WeaponGameState || WeaponGameState->IsGameplayRecoveryBlocked() ||
		WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::InGame)
	{
		return;
	}

	GetWorldTimerManager().ClearTimer(MatchPhaseTimer);
	WeaponGameState->SetMissionOutcomeAuthority(Outcome);
	WeaponGameState->SetMatchPhase(EWeaponMatchPhase::Ending, EndingDuration);

	// 结算期不再流血致死:清掉全部流血计时(倒地者随后走下面的免费复活,RespawnPlayer 会恢复为 Up)。
	for (auto& BleedPair : BleedOutTimers)
	{
		GetWorldTimerManager().ClearTimer(BleedPair.Value);
		if (AWeaponPlayerState* BleedPS = BleedPair.Key ? BleedPair.Key->GetPlayerState<AWeaponPlayerState>() : nullptr)
		{
			BleedPS->SetDownedServerTime(0.0f);
		}
	}
	BleedOutTimers.Empty();

	// 局内死亡且未排复活(池空)的玩家:离开 InGame 后恢复"死亡 → RespawnDelay → 免费复活"的旧行为,
	// 否则他们会带着尸体进入下一局。放在结算开始而非回到 Waiting,保证下一局开始前已复活。
	for (APlayerState* PlayerState : WeaponGameState->PlayerArray)
	{
		AController* PlayerController = PlayerState ? PlayerState->GetOwningController() : nullptr;
		if (PlayerController && WeaponGetLifeState(PlayerState) != EWeaponPlayerLifeState::Up &&
			!RespawnTimers.Contains(PlayerController))
		{
			ArmRespawnTimer(PlayerController, RespawnDelay);
		}
	}

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
	if (!WeaponGameState || WeaponGameState->IsGameplayRecoveryBlocked() ||
		WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::Ending)
	{
		return;
	}

	DepartedLifeStates.Empty();
	ResetAllPlayerReadyStates();
	CleanupObjectiveActors();
	if (WaveDirector)
	{
		WaveDirector->ResetWaves(true);
	}
	WeaponGameState->SetMissionOutcomeAuthority(EWeaponMissionOutcome::None);
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

void AWeaponGameMode::RespawnPlayerImmediately(AController* PlayerController)
{
	if (!PlayerController)
	{
		return;
	}

	// 局内:已倒下/死亡的玩家不能借职业选择绕开增援池(只能等自动增援或被救起)。
	if (const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
		WeaponGameState && WeaponGameState->GetMatchPhase() == EWeaponMatchPhase::InGame &&
		WeaponGetLifeState(PlayerController->GetPlayerState<APlayerState>()) != EWeaponPlayerLifeState::Up)
	{
		return;
	}

	// 若当前存在挂起的死亡重生计时器，先清除，避免稍后重复触发一次 RespawnPlayer
	if (FTimerHandle* RespawnTimer = RespawnTimers.Find(PlayerController))
	{
		GetWorldTimerManager().ClearTimer(*RespawnTimer);
		RespawnTimers.Remove(PlayerController);
	}

	RespawnPlayer(PlayerController);
}

void AWeaponGameMode::RespawnPlayer(AController* PlayerController)
{
	if (!PlayerController || AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}

	// 0. 倒地者被复活/增援:清掉流血计时与时间戳
	ClearBleedOut(PlayerController);

	// 1. 销毁旧 Pawn（如果还存在）
	if (APawn* OldPawn = PlayerController->GetPawn())
	{
		OldPawn->Destroy();
	}

	// 2. 通过统一的 RestartPlayer 路径选择 PlayerStart 或按 Slot 使用安全 fallback。
	RestartPlayer(PlayerController);
	if (AWeaponPlayerState* RespawnedPS = PlayerController->GetPlayerState<AWeaponPlayerState>())
	{
		RespawnedPS->SetRespawnServerTime(0.0f);
		RespawnedPS->SetLifeStateAuthority(EWeaponPlayerLifeState::Up);
		// State.Downed 是 PlayerState ASC 上的松散 Tag,不随旧 Pawn 销毁,必须显式清除。
		if (UAbilitySystemComponent* RespawnASC = RespawnedPS->GetAbilitySystemComponent();
			RespawnASC && RespawnASC->HasMatchingGameplayTag(TAG_State_Downed))
		{
			RespawnASC->RemoveLooseGameplayTag(TAG_State_Downed);
		}
	}
	APawn* NewPawn = PlayerController->GetPawn();
	if (!NewPawn)
	{
		return;
	}

	// 3. 恢复玩家状态（RestartPlayer 已 Possess，ASC ActorInfo 已更新到新 Pawn）
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

	if (!WeaponGameState || WeaponGameState->IsGameplayRecoveryBlocked())
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

void AWeaponGameMode::CaptureObjectiveSnapshot(FWeaponObjectiveSnapshot& OutSnapshot) const
{
	OutSnapshot = FWeaponObjectiveSnapshot();
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!WeaponGameState || !ObjectiveActor || !ExtractionActor)
	{
		return;
	}

	const FWeaponObjectiveState& State = WeaponGameState->GetObjectiveState();
	OutSnapshot.bActorsSpawned = true;
	OutSnapshot.bObjectiveComplete = State.bObjectiveComplete;
	OutSnapshot.bExtractionOpen = State.bExtractionOpen;
	OutSnapshot.ObjectiveChargeProgress = ObjectiveActor->GetChargeProgress();
	OutSnapshot.ExtractionHoldProgress = ExtractionActor->GetHoldProgress();
	if (State.bObjectiveComplete && !State.bExtractionOpen)
	{
		// 以 Timer 为准(绝对时间戳只是它的复制投影)。
		const FTimerManager& TimerManager = GetWorldTimerManager();
		OutSnapshot.ExtractionRemainingSeconds = TimerManager.TimerExists(ExtractionTimer)
			? FMath::Max(0.0f, TimerManager.GetTimerRemaining(ExtractionTimer))
			: 0.0f;
	}
}

void AWeaponGameMode::RestoreObjectiveSnapshot(const FWeaponObjectiveSnapshot& Snapshot)
{
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!HasAuthority() || !WeaponGameState || !Snapshot.bActorsSpawned)
	{
		return;
	}

	// Recipe 已在 Apply 前恢复,marker 位置与原世界一致。
	SpawnObjectiveActors(true);
	if (!ObjectiveActor || !ExtractionActor)
	{
		UE_LOG(LogLetsDeepRock, Warning, TEXT("[A1] Recovery could not respawn objective/extraction actors"));
		return;
	}
	ObjectiveActor->RestoreProgress(Snapshot.ObjectiveChargeProgress, Snapshot.bObjectiveComplete);
	ExtractionActor->RestoreState(Snapshot.bExtractionOpen, Snapshot.ExtractionHoldProgress);

	FWeaponObjectiveState State;
	State.bObjectiveComplete = Snapshot.bObjectiveComplete;
	State.bExtractionOpen = Snapshot.bExtractionOpen;
	if (Snapshot.bObjectiveComplete && !Snapshot.bExtractionOpen)
	{
		const float Remaining = FMath::Max(Snapshot.ExtractionRemainingSeconds, 0.1f);
		State.ExtractionReadyServerTime = WeaponGameState->GetServerWorldTimeSeconds() + Remaining;
		GetWorldTimerManager().SetTimer(ExtractionTimer, this, &AWeaponGameMode::OpenExtraction, Remaining, false);
		// 屏障释放时按暂停中的剩余时间续跑并重新锚定 ExtractionReadyServerTime。
		if (WeaponGameState->IsGameplayRecoveryBlocked())
		{
			GetWorldTimerManager().PauseTimer(ExtractionTimer);
		}
	}
	WeaponGameState->SetObjectiveStateAuthority(State);
}

void AWeaponGameMode::RestorePendingRespawn(AController* PlayerController, const float DelaySeconds)
{
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!HasAuthority() || !PlayerController || !WeaponGameState)
	{
		return;
	}
	ArmRespawnTimer(PlayerController, FMath::Max(DelaySeconds, 0.1f));
	if (WeaponGameState->IsGameplayRecoveryBlocked())
	{
		if (FTimerHandle* RespawnTimer = RespawnTimers.Find(PlayerController))
		{
			GetWorldTimerManager().PauseTimer(*RespawnTimer);
		}
	}
}
