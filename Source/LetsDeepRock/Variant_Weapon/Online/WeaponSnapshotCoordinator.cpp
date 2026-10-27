#include "WeaponSnapshotCoordinator.h"
#include "LetsDeepRock.h"
#include "AbilitySystemComponent.h"
#include "AttributeModifierManager.h"
#include "Attributes/StatusEffectComponent.h"
#include "AI/WeaponWaveDirectorComponent.h"
#include "EnemyCharacter.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameDataSubsystem.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "GameplayEffect.h"
#include "GridMap/GridMapGenerator.h"
#include "GridMap/GridMapManager.h"
#include "Kismet/GameplayStatics.h"
#include "Serialization/MemoryReader.h"
#include "Swarm/SwarmTypes.h"
#include "WeaponCharacter.h"
#include "WeaponGameMode.h"
#include "WeaponGameState.h"
#include "WeaponMatchSaveGame.h"
#include "WeaponPlayerController.h"
#include "WeaponPlayerState.h"
#include "WeaponGameplayTags.h"
#include "WeaponSnapshot.h"
#include "Engine/GameInstance.h"
#include "UObject/Package.h"
#include "Weapons/GrenadeProjectile.h"
#include "Weapons/WeaponEquipmentComponent.h"
#include "Weapons/WeaponDataAsset.h"
#include "Weapons/WeaponGrenadeDataAsset.h"
#include "Weapons/WeaponProjectile.h"

namespace WeaponSnapshotSerialization
{
	/** UE 5.8's modern SaveGame header tag ("SAVG"). Legacy untagged saves are not migration payloads. */
	constexpr int32 SaveGameFileTypeTag = 0x53415647;
}

struct UWeaponSnapshotCoordinator::FRestoreContext
{
	struct FPlayerTarget
	{
		const FWeaponPlayerSnapshot* Snapshot = nullptr;
		AWeaponPlayerState* PlayerState = nullptr;
		APlayerController* PlayerController = nullptr;
		APawn* ExistingPawn = nullptr;
		AWeaponCharacter* Character = nullptr;
		UClass* PawnClass = nullptr;
		UWeaponEquipmentComponent* Equipment = nullptr;
		FWeaponEquipmentSnapshotState EquipmentSnapshot;
		UAbilitySystemComponent* ASC = nullptr;
		bool bCharacterStaged = false;
		bool bPlayerStateStaged = false;
		bool bAttributionTombstone = false;
	};

	UWorld* World = nullptr;
	AWeaponGameState* GameState = nullptr;
	AWeaponGameMode* GameMode = nullptr;
	AGridMapManager* GridMap = nullptr;
	UClass* GridGeneratorClass = nullptr;
	UClass* PlayerStateClass = nullptr;

	TArray<FPlayerTarget> Players;
	TMap<FUniqueNetIdRepl, APawn*> FinalPawnsByPlayerId;
	TMap<FUniqueNetIdRepl, AWeaponPlayerState*> ConnectedPlayersById;

	TArray<UClass*> EnemyClasses;
	TArray<AEnemyCharacter*> ExistingEnemies;
	TArray<AEnemyCharacter*> StagedEnemies;

	TArray<UClass*> ProjectileClasses;
	TArray<TSubclassOf<UGameplayEffect>> ProjectileDamageEffects;
	TArray<const UWeaponGrenadeDataAsset*> GrenadeDataAssets;
	TArray<AWeaponProjectile*> ExistingProjectiles;
	TArray<AWeaponProjectile*> StagedProjectiles;
};

FGuid UWeaponSnapshotCoordinator::GetOrCreateActiveMatchId()
{
	if (!ActiveMatchId.IsValid())
	{
		ActiveMatchId = FGuid::NewGuid();
	}
	return ActiveMatchId;
}

void UWeaponSnapshotCoordinator::ClearActiveMatchId()
{
	ActiveMatchId.Invalidate();
	NextSnapshotRevision = 1;
}

bool UWeaponSnapshotCoordinator::AdoptActiveMatchId(
	const FGuid& MatchId,
	FString& OutError)
{
	OutError.Reset();
	if (!MatchId.IsValid())
	{
		OutError = TEXT("Coordinator MatchId is invalid");
		return false;
	}
	if (ActiveMatchId.IsValid() && ActiveMatchId != MatchId)
	{
		OutError = TEXT("A different logical MatchId is already active");
		return false;
	}
	if (NextSnapshotRevision != 1)
	{
		OutError = TEXT("Coordinator MatchId must be adopted before the first Snapshot capture");
		return false;
	}

	ActiveMatchId = MatchId;
	return true;
}

bool UWeaponSnapshotCoordinator::CaptureSaveGame(
	const UWorld* World,
	UWeaponMatchSaveGame*& OutSaveGame,
	FString& OutError)
{
	OutSaveGame = nullptr;
	OutError.Reset();
	bHasActiveRecoverySnapshot = false;
	SnapshotAppliedPlayerIds.Reset();

	if (!World || World->GetNetMode() == NM_Client)
	{
		OutError = TEXT("SaveGame could only be captured from a valid server World");
		return false;
	}

	const AWeaponGameState* GameState = World->GetGameState<AWeaponGameState>();
	if (!GameState)
	{
		OutError = TEXT("Current World does not have AWeaponGameState");
		return false;
	}
	if (GameState->IsGameplayRecoveryBlocked())
	{
		OutError = TEXT("A new SaveGame cannot be captured while gameplay recovery is blocked");
		return false;
	}

	UWeaponMatchSaveGame* SaveGame = Cast<UWeaponMatchSaveGame>(
		UGameplayStatics::CreateSaveGameObject(UWeaponMatchSaveGame::StaticClass()));
	if (!SaveGame)
	{
		OutError = TEXT("Failed to create UWeaponMatchSaveGame");
		return false;
	}

	SaveGame->MapPackageName = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
	SaveGame->MaxPlayers = GameState->GetMaxPlayers();
	if (!CaptureSnapshot(World, SaveGame->Snapshot, OutError) || !ValidateSaveGame(SaveGame, OutError))
	{
		return false;
	}

	RetainedSaveGame = SaveGame;
	OutSaveGame = SaveGame;
	return true;
}

bool UWeaponSnapshotCoordinator::ValidateSaveGame(
	const UWeaponMatchSaveGame* SaveGame,
	FString& OutError)
{
	OutError.Reset();
	if (!SaveGame || SaveGame->SaveFormatVersion != WeaponMatchSaveGameVersion)
	{
		OutError = TEXT("SaveGame object or format version is invalid");
		return false;
	}

	if (SaveGame->MapPackageName.IsEmpty() || SaveGame->MaxPlayers < 1 || SaveGame->MaxPlayers > 4)
	{
		OutError = TEXT("SaveGame map package or player capacity is invalid");
		return false;
	}

	return ValidateSnapshot(SaveGame->Snapshot, SaveGame->MaxPlayers, OutError);
}

bool UWeaponSnapshotCoordinator::RestoreSaveGame(
	const UWeaponMatchSaveGame* SaveGame,
	FString& OutError)
{
	bHasActiveRecoverySnapshot = false;
	SnapshotAppliedPlayerIds.Reset();
	FRestoreContext Context;
	if (!ValidateRestore(SaveGame, Context, OutError))
	{
		return false;
	}

	if (!StageRestoreActors(SaveGame->Snapshot, Context, OutError))
	{
		CleanupStagedActors(Context);
		return false;
	}

	// GridMap Restore can still reject authority or a missing loaded generator, but it mutates only after
	// those checks. Keep it immediately after Actor staging so failure can clean staged actors while every
	// pre-existing gameplay actor remains untouched.
	if (!Context.GridMap->RestoreRecoveryState(SaveGame->Snapshot.GridMap, OutError))
	{
		CleanupStagedActors(Context);
		return false;
	}

	ApplySnapshot(SaveGame, Context);
	bHasActiveRecoverySnapshot = true;
	return true;
}

void UWeaponSnapshotCoordinator::ApplyPlayerLifeState(
	AWeaponGameMode* GameMode,
	AController* Controller,
	AWeaponPlayerState* PlayerState,
	AWeaponCharacter* Character,
	const FWeaponPlayerSnapshot& Player,
	const EWeaponMatchPhase Phase)
{
	if (!GameMode || !PlayerState)
	{
		return;
	}

	// 显式写回生存状态:PlayerState 默认的 Up 不能泄漏到死亡/倒地玩家,并补齐服务端 GAS Tag。
	// 倒地规则只在 InGame 有效:Ending 快照里的倒地者(流血计时已被结算清掉)按死亡恢复,
	// 并沿用其待复活计时,而不是被还原成 0.1 s 流血的倒地状态。
	EWeaponPlayerLifeState EffectiveLifeState = Player.LifeState;
	if (EffectiveLifeState == EWeaponPlayerLifeState::Downed && Phase != EWeaponMatchPhase::InGame)
	{
		EffectiveLifeState = EWeaponPlayerLifeState::Dead;
	}
	switch (EffectiveLifeState)
	{
	case EWeaponPlayerLifeState::Downed:
		if (Character && Controller)
		{
			// EnterDowned + State.Downed + 流血 Timer(屏障内暂停,Release 时重新锚定)
			GameMode->RestoreDownedPlayer(Controller, Player.DownedRemainingSeconds);
			return;
		}
		break; // 倒地却无 Pawn 的异常数据:按存活无 Pawn 兜底处理
	case EWeaponPlayerLifeState::Dead:
		// EnterDeath 写 State.Dead + LifeState=Dead,并隐藏/禁碰撞/卸武器。
		if (Character)
		{
			Character->EnterDeath();
		}
		PlayerState->SetLifeStateAuthority(EWeaponPlayerLifeState::Dead);
		// 复活 Timer 在源世界存在才重建(与阶段无关,保持"其他阶段沿用旧行为");池空而死(未排复活)保持死亡。
		if (Player.bRespawnPending && Controller)
		{
			GameMode->RestorePendingRespawn(Controller, Player.RespawnRemainingSeconds);
		}
		return;
	default:
		break;
	}

	if (Character)
	{
		UAbilitySystemComponent* ASC = PlayerState->GetAbilitySystemComponent();
		if (ASC && ASC->HasMatchingGameplayTag(TAG_State_Downed))
		{
			Character->ExitDowned();
		}
	}
	PlayerState->SetLifeStateAuthority(EWeaponPlayerLifeState::Up);
	// 兜底:快照里为 Up 却在 InGame 中没有 Pawn(如捕获时尚未出生的中途加入玩家,或旧 v10 数据里的"死亡待复活")。
	// 恢复世界没有原 Timer,按默认延迟重建(屏障内暂停);到点由 TryReinforcePlayer 免费补出 Pawn(不扣池)。
	if (!Character && Phase == EWeaponMatchPhase::InGame && Controller)
	{
		GameMode->RestorePendingRespawn(Controller, GameMode->GetRespawnDelay());
	}
}

bool UWeaponSnapshotCoordinator::RestoreReconnectedPlayer(
	AWeaponPlayerController* PlayerController,
	FString& OutError)
{
	OutError.Reset();
	if (!PlayerController || !PlayerController->HasAuthority() || !RetainedSaveGame ||
		!bHasActiveRecoverySnapshot || !AWeaponGameState::IsGameplayRecoveryBlockedFor(PlayerController))
	{
		OutError = TEXT("Reconnect restore requires an active gated recovery Snapshot");
		return false;
	}

	AWeaponPlayerState* PlayerState = PlayerController->GetPlayerState<AWeaponPlayerState>();
	AWeaponCharacter* Character = Cast<AWeaponCharacter>(PlayerController->GetPawn());
	if (!PlayerState || !Character || !PlayerState->GetUniqueId().IsValid())
	{
		OutError = TEXT("Reconnect restore requires a valid PlayerState and possessed WeaponCharacter");
		return false;
	}

	const FUniqueNetIdRepl PlayerId = PlayerState->GetUniqueId();
	if (SnapshotAppliedPlayerIds.Contains(PlayerId))
	{
		return true;
	}

	const FWeaponPlayerSnapshot* PlayerSnapshot = RetainedSaveGame->Snapshot.Players.FindByPredicate(
		[&PlayerId](const FWeaponPlayerSnapshot& Candidate)
		{
			return Candidate.PlayerId == PlayerId;
		});
	if (!PlayerSnapshot)
	{
		OutError = FString::Printf(TEXT("Reconnect restore has no Snapshot player for %s"), *PlayerId.ToDebugString());
		return false;
	}
	AWeaponGameMode* GameMode = PlayerController->GetWorld()
		? PlayerController->GetWorld()->GetAuthGameMode<AWeaponGameMode>()
		: nullptr;
	const EWeaponMatchPhase SnapshotPhase = RetainedSaveGame->Snapshot.MatchPhase;
	if (!PlayerSnapshot->bHasPawn)
	{
		// 快照中该玩家无 Pawn(死亡):RestartPlayer 给了活 Pawn,这里让它死亡并按同一规则排复活。
		if (GameMode)
		{
			ApplyPlayerLifeState(GameMode, PlayerController, PlayerState, Character, *PlayerSnapshot, SnapshotPhase);
		}
		SnapshotAppliedPlayerIds.Add(PlayerId);
		return true;
	}

	if (!PlayerState->SetProfessionLoadout(
		PlayerSnapshot->ProfessionId,
		PlayerSnapshot->PrimaryWeaponId,
		PlayerSnapshot->SecondaryWeaponId))
	{
		OutError = FString::Printf(TEXT("Failed to apply reconnect profession loadout for %s"), *PlayerId.ToDebugString());
		return false;
	}
	PlayerState->SetPlayerName(PlayerSnapshot->PlayerName);
	PlayerState->SetPlayerReady(PlayerSnapshot->bIsReady);
	PlayerState->SetTeamId(PlayerSnapshot->TeamId);
	Character->RefreshProfessionLoadout();

	UWeaponEquipmentComponent* Equipment = Character->FindComponentByClass<UWeaponEquipmentComponent>();
	if (!Equipment || PlayerSnapshot->WeaponInventory.Num() != PlayerSnapshot->WeaponAmmoStates.Num())
	{
		OutError = FString::Printf(TEXT("Reconnect equipment state is invalid for %s"), *PlayerId.ToDebugString());
		return false;
	}
	FWeaponEquipmentSnapshotState EquipmentSnapshot;
	EquipmentSnapshot.CurrentWeaponIndex = PlayerSnapshot->CurrentWeaponIndex;
	EquipmentSnapshot.bWasReloading = PlayerSnapshot->bWasReloading;
	for (const FSoftObjectPath& WeaponPath : PlayerSnapshot->WeaponInventory)
	{
		UWeaponDataAsset* Weapon = Cast<UWeaponDataAsset>(WeaponPath.TryLoad());
		if (!Weapon)
		{
			OutError = FString::Printf(TEXT("Failed to load reconnect weapon %s for %s"), *WeaponPath.ToString(), *PlayerId.ToDebugString());
			return false;
		}
		EquipmentSnapshot.WeaponInventory.Add(Weapon);
	}
	for (const FWeaponAmmoSnapshot& SavedAmmo : PlayerSnapshot->WeaponAmmoStates)
	{
		FWeaponAmmoState& Ammo = EquipmentSnapshot.WeaponAmmoStates.AddDefaulted_GetRef();
		Ammo.MagazineAmmo = SavedAmmo.MagazineAmmo;
		Ammo.ReserveAmmo = SavedAmmo.ReserveAmmo;
	}
	if (!Equipment->RestoreSnapshotState(EquipmentSnapshot) ||
		!Character->RestoreEquipmentSlot(PlayerSnapshot->CurrentEquipmentSlot))
	{
		OutError = FString::Printf(TEXT("Failed to apply reconnect equipment for %s"), *PlayerId.ToDebugString());
		return false;
	}

	Character->SetActorLocationAndRotation(
		PlayerSnapshot->Location,
		PlayerSnapshot->Rotation,
		false,
		nullptr,
		ETeleportType::TeleportPhysics);
	Character->SetAimDirectionAuthoritative(PlayerSnapshot->Rotation.Vector());
	Character->GetCharacterMovement()->Velocity = PlayerSnapshot->Velocity;
	RestoreAttributeValues(PlayerState->GetAbilitySystemComponent(), PlayerSnapshot->AttributeState.Values);
	Character->ForceNetUpdate();
	if (GameMode)
	{
		ApplyPlayerLifeState(GameMode, PlayerController, PlayerState, Character, *PlayerSnapshot, SnapshotPhase);
	}
	SnapshotAppliedPlayerIds.Add(PlayerId);
	UE_LOG(LogLetsDeepRock, Log, TEXT("Applied reconnect Snapshot Player=%s Location=%s Yaw=%.1f WeaponIndex=%d"),
		*PlayerId.ToDebugString(),
		*PlayerSnapshot->Location.ToCompactString(),
		PlayerSnapshot->Rotation.Yaw,
		PlayerSnapshot->CurrentWeaponIndex);
	return true;
}

bool UWeaponSnapshotCoordinator::SerializeSaveGame(
	UWeaponMatchSaveGame* SaveGame,
	TArray<uint8>& OutData,
	FString& OutError)
{
	OutData.Reset();
	if (!ValidateSaveGame(SaveGame, OutError))
	{
		return false;
	}

	if (!UGameplayStatics::SaveGameToMemory(SaveGame, OutData))
	{
		OutError = TEXT("Failed to serialize UWeaponMatchSaveGame to memory");
		return false;
	}
	return true;
}

bool UWeaponSnapshotCoordinator::DeserializeSaveGame(
	const TArray<uint8>& Data,
	UWeaponMatchSaveGame*& OutSaveGame,
	FString& OutError)
{
	OutSaveGame = nullptr;
	OutError.Reset();
	if (Data.IsEmpty())
	{
		OutError = TEXT("SaveGame byte buffer is empty");
		return false;
	}
	if (Data.Num() < sizeof(int32))
	{
		OutError = TEXT("SaveGame byte buffer is too small to contain a modern header");
		return false;
	}

	FMemoryReader HeaderReader(Data, true);
	int32 FileTypeTag = 0;
	HeaderReader << FileTypeTag;
	if (HeaderReader.IsError() ||
		FileTypeTag != WeaponSnapshotSerialization::SaveGameFileTypeTag)
	{
		OutError = TEXT("SaveGame byte buffer does not contain a modern SAVG header");
		return false;
	}

	UWeaponMatchSaveGame* SaveGame = Cast<UWeaponMatchSaveGame>(UGameplayStatics::LoadGameFromMemory(Data));
	if (!SaveGame)
	{
		OutError = TEXT("SaveGame byte buffer does not contain UWeaponMatchSaveGame");
		return false;
	}

	if (!ValidateSaveGame(SaveGame, OutError))
	{
		return false;
	}

	RetainedSaveGame = SaveGame;
	OutSaveGame = SaveGame;
	return true;
}

bool UWeaponSnapshotCoordinator::ValidateRestore(
	const UWeaponMatchSaveGame* SaveGame,
	FRestoreContext& OutContext,
	FString& OutError) const
{
	OutContext = FRestoreContext();
	if (!ValidateSaveGame(SaveGame, OutError))
	{
		return false;
	}

	UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client)
	{
		OutError = TEXT("SaveGame could only be restored into a valid server World");
		return false;
	}

	const FString CurrentMapPackage = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName());
	if (CurrentMapPackage != SaveGame->MapPackageName)
	{
		OutError = FString::Printf(
			TEXT("SaveGame requires map %s but current map is %s"),
			*SaveGame->MapPackageName,
			*CurrentMapPackage);
		return false;
	}
	OutContext.World = World;
	OutContext.GameState = World->GetGameState<AWeaponGameState>();
	OutContext.GameMode = World->GetAuthGameMode<AWeaponGameMode>();
	if (!OutContext.GameState || !OutContext.GameMode ||
		!OutContext.GameState->IsGameplayRecoveryBlocked() ||
		OutContext.GameState->PlayerArray.Num() > SaveGame->MaxPlayers)
	{
		OutError = TEXT("Restore target is not a gated authoritative Recovery World or exceeds saved capacity");
		return false;
	}

	const FWeaponSnapshot& Snapshot = SaveGame->Snapshot;
	int32 GridMapCount = 0;
	for (TActorIterator<AGridMapManager> It(World); It; ++It)
	{
		OutContext.GridMap = *It;
		++GridMapCount;
	}
	if (GridMapCount != 1)
	{
		OutError = TEXT("Snapshot Restore requires exactly one GridMapManager");
		return false;
	}

	OutContext.GridGeneratorClass = Snapshot.GridMap.GeneratorClass.TryLoadClass<UGridMapGenerator>();
	if (!OutContext.GridGeneratorClass)
	{
		OutError = FString::Printf(TEXT("GridMap generator class %s could not be loaded"),
			*Snapshot.GridMap.GeneratorClass.ToString());
		return false;
	}

	const UGameInstance* GameInstance = GetGameInstance();
	const UGameDataSubsystem* DataSubsystem = GameInstance
		? GameInstance->GetSubsystem<UGameDataSubsystem>()
		: nullptr;
	if (!DataSubsystem)
	{
		OutError = TEXT("GameDataSubsystem is unavailable during Restore validation");
		return false;
	}

	TMap<FUniqueNetIdRepl, const FWeaponRecoveryRosterSnapshot*> RosterById;
	for (const FWeaponRecoveryRosterSnapshot& RosterEntry : Snapshot.RecoveryRoster)
	{
		RosterById.Add(RosterEntry.PlayerId, &RosterEntry);
	}

	for (APlayerState* BasePlayerState : OutContext.GameState->PlayerArray)
	{
		AWeaponPlayerState* PlayerState = Cast<AWeaponPlayerState>(BasePlayerState);
		const FUniqueNetIdRepl PlayerId = ResolvePlayerId(PlayerState);
		const FWeaponRecoveryRosterSnapshot* const* RosterEntry = RosterById.Find(PlayerId);
		if (!PlayerState || !PlayerId.IsValid() || OutContext.ConnectedPlayersById.Contains(PlayerId) ||
			!RosterEntry || (*RosterEntry)->Status == EWeaponRecoveryPlayerStatus::Departed)
		{
			OutError = TEXT("Restore target contains a PlayerState outside the saved active roster");
			return false;
		}
		OutContext.ConnectedPlayersById.Add(PlayerId, PlayerState);
	}

	TSet<FUniqueNetIdRepl> ProjectileSourceIds;
	for (const FWeaponProjectileSnapshot& Projectile : Snapshot.Projectiles)
	{
		ProjectileSourceIds.Add(Projectile.InstigatorPlayerId);
	}
	for (const FWeaponGrenadeSnapshot& Grenade : Snapshot.Grenades)
	{
		ProjectileSourceIds.Add(Grenade.InstigatorPlayerId);
	}

	OutContext.Players.Reserve(Snapshot.Players.Num());
	TSet<FUniqueNetIdRepl> RestorableSourceIds;
	for (const FWeaponPlayerSnapshot& Player : Snapshot.Players)
	{
		AWeaponPlayerState* const* FoundPlayerState = OutContext.ConnectedPlayersById.Find(Player.PlayerId);
		const bool bNeedsAttributionTombstone = !FoundPlayerState && ProjectileSourceIds.Contains(Player.PlayerId);
		if (!FoundPlayerState && !bNeedsAttributionTombstone)
		{
			continue;
		}

		FRestoreContext::FPlayerTarget& Target = OutContext.Players.AddDefaulted_GetRef();
		Target.Snapshot = &Player;
		Target.bAttributionTombstone = bNeedsAttributionTombstone;
		if (FoundPlayerState)
		{
			Target.PlayerState = *FoundPlayerState;
			Target.PlayerController = Target.PlayerState->GetPlayerController();
			Target.ExistingPawn = Target.PlayerController
				? Target.PlayerController->GetPawn().Get()
				: Target.PlayerState->GetPawn();
			Target.ASC = Target.PlayerState->GetAbilitySystemComponent();
			if (!CanRestoreAttributeValues(Target.ASC, Player.AttributeState.Values))
			{
				OutError = FString::Printf(TEXT("Player %s ASC is incompatible with Snapshot attributes"),
					*Player.PlayerId.ToDebugString());
				return false;
			}
		}
		const bool bHasProfessionLoadout = !Player.ProfessionId.IsNone() || !Player.PrimaryWeaponId.IsNone() || !Player.SecondaryWeaponId.IsNone();
		if ((Player.bHasPawn || bHasProfessionLoadout) &&
			(!DataSubsystem->GetProfessionConfig(Player.ProfessionId) ||
			!DataSubsystem->GetProfessionWeaponIds(Player.ProfessionId, TEXT("Primary")).Contains(Player.PrimaryWeaponId) ||
			!DataSubsystem->GetProfessionWeaponIds(Player.ProfessionId, TEXT("Secondary")).Contains(Player.SecondaryWeaponId)))
		{
			OutError = FString::Printf(TEXT("Profession loadout for player %s is invalid"),
				*Player.PlayerId.ToDebugString());
			return false;
		}
		if (!Player.bHasPawn)
		{
			continue;
		}
		if (!Target.PlayerController && !Target.bAttributionTombstone)
		{
			OutError = FString::Printf(TEXT("Player %s does not have a PlayerController"),
				*Player.PlayerId.ToDebugString());
			return false;
		}

		for (const FSoftObjectPath& WeaponPath : Player.WeaponInventory)
		{
			UWeaponDataAsset* Weapon = Cast<UWeaponDataAsset>(WeaponPath.TryLoad());
			if (!Weapon)
			{
				OutError = FString::Printf(TEXT("Failed to load equipment for player %s"),
					*Player.PlayerId.ToDebugString());
				return false;
			}
			Target.EquipmentSnapshot.WeaponInventory.Add(Weapon);
		}
		for (const FWeaponAmmoSnapshot& SavedAmmo : Player.WeaponAmmoStates)
		{
			FWeaponAmmoState& Ammo = Target.EquipmentSnapshot.WeaponAmmoStates.AddDefaulted_GetRef();
			Ammo.MagazineAmmo = SavedAmmo.MagazineAmmo;
			Ammo.ReserveAmmo = SavedAmmo.ReserveAmmo;
		}
		Target.EquipmentSnapshot.CurrentWeaponIndex = Player.CurrentWeaponIndex;
		Target.EquipmentSnapshot.bWasReloading = Player.bWasReloading;

		if (!DataSubsystem->GetCharacterAttributes(Player.CharacterConfigRowName))
		{
			OutError = FString::Printf(TEXT("Character config row %s does not exist"),
				*Player.CharacterConfigRowName.ToString());
			return false;
		}

		Target.Character = Cast<AWeaponCharacter>(Target.ExistingPawn);
		if (Target.Character)
		{
			Target.Equipment = Target.Character->FindComponentByClass<UWeaponEquipmentComponent>();
			if (!Target.Character->GetCharacterMovement() || !Target.Equipment)
			{
				OutError = FString::Printf(TEXT("Existing Character for player %s is not Restore-compatible"),
					*Player.PlayerId.ToDebugString());
				return false;
			}
			OutContext.FinalPawnsByPlayerId.Add(Player.PlayerId, Target.Character);
		}
		else
		{
			Target.PawnClass = OutContext.GameMode->GetDefaultPawnClassForController(Target.PlayerController);
			if (!Target.PawnClass || Target.PawnClass->HasAnyClassFlags(CLASS_Abstract) ||
				!Target.PawnClass->IsChildOf(AWeaponCharacter::StaticClass()))
			{
				OutError = FString::Printf(TEXT("Player %s has no restorable default Character class"),
					*Player.PlayerId.ToDebugString());
				return false;
			}
		}
		RestorableSourceIds.Add(Player.PlayerId);
	}

	if (!ProjectileSourceIds.IsEmpty())
	{
		OutContext.PlayerStateClass = OutContext.GameMode->PlayerStateClass;
		if (!OutContext.PlayerStateClass || OutContext.PlayerStateClass->HasAnyClassFlags(CLASS_Abstract) ||
			!OutContext.PlayerStateClass->IsChildOf(AWeaponPlayerState::StaticClass()))
		{
			OutError = TEXT("Restore target has no restorable WeaponPlayerState class");
			return false;
		}
	}
	for (const FUniqueNetIdRepl& SourceId : ProjectileSourceIds)
	{
		if (!RestorableSourceIds.Contains(SourceId))
		{
			OutError = FString::Printf(TEXT("Projectile source %s has no restorable player context"),
				*SourceId.ToDebugString());
			return false;
		}
	}

	OutContext.EnemyClasses.Reserve(Snapshot.Enemies.Num());
	for (const FWeaponEnemySnapshot& Enemy : Snapshot.Enemies)
	{
		UClass* EnemyClass = Enemy.EnemyClass.TryLoadClass<AEnemyCharacter>();
		if (!EnemyClass || EnemyClass->HasAnyClassFlags(CLASS_Abstract) ||
			!DataSubsystem->GetEnemyConfig(Enemy.EnemyConfigRowName))
		{
			OutError = FString::Printf(TEXT("Enemy class or config %s could not be loaded"),
				*Enemy.EnemyConfigRowName.ToString());
			return false;
		}
		OutContext.EnemyClasses.Add(EnemyClass);
	}
	for (TActorIterator<AEnemyCharacter> It(World); It; ++It)
	{
		OutContext.ExistingEnemies.Add(*It);
	}

	OutContext.ProjectileClasses.Reserve(Snapshot.Projectiles.Num());
	OutContext.ProjectileDamageEffects.Reserve(Snapshot.Projectiles.Num());
	for (const FWeaponProjectileSnapshot& Projectile : Snapshot.Projectiles)
	{
		UClass* ProjectileClass = Projectile.ProjectileClass.TryLoadClass<AWeaponProjectile>();
		TSubclassOf<UGameplayEffect> DamageEffectClass =
			Projectile.DamageEffectClass.TryLoadClass<UGameplayEffect>();
		if (!ProjectileClass || ProjectileClass->HasAnyClassFlags(CLASS_Abstract) ||
			ProjectileClass->IsChildOf(AGrenadeProjectile::StaticClass()) ||
			!DamageEffectClass)
		{
			OutError = FString::Printf(TEXT("Projectile class or damage effect %s could not be loaded"),
				*Projectile.ProjectileClass.ToString());
			return false;
		}
		OutContext.ProjectileClasses.Add(ProjectileClass);
		OutContext.ProjectileDamageEffects.Add(DamageEffectClass);
	}

	OutContext.GrenadeDataAssets.Reserve(Snapshot.Grenades.Num());
	for (const FWeaponGrenadeSnapshot& Grenade : Snapshot.Grenades)
	{
		const UWeaponGrenadeDataAsset* GrenadeData = Cast<UWeaponGrenadeDataAsset>(
			Grenade.GrenadeDataAsset.TryLoad());
		if (!GrenadeData || !GrenadeData->ProjectileClass ||
			GrenadeData->ProjectileClass->HasAnyClassFlags(CLASS_Abstract) ||
			!GrenadeData->ProjectileClass->IsChildOf(AGrenadeProjectile::StaticClass()))
		{
			OutError = FString::Printf(TEXT("Grenade DataAsset %s could not be loaded"),
				*Grenade.GrenadeDataAsset.ToString());
			return false;
		}
		OutContext.GrenadeDataAssets.Add(GrenadeData);
	}
	for (TActorIterator<AWeaponProjectile> It(World); It; ++It)
	{
		if (!It->IsActorBeingDestroyed())
		{
			OutContext.ExistingProjectiles.Add(*It);
		}
	}
	return true;
}

bool UWeaponSnapshotCoordinator::CaptureSnapshot(
	const UWorld* World,
	FWeaponSnapshot& OutSnapshot,
	FString& OutError)
{
	OutError.Reset();
	if (!World || World->GetNetMode() == NM_Client)
	{
		OutError = TEXT("Snapshot could only be captured from a valid server World");
		return false;
	}

	const AWeaponGameState* GameState = World->GetGameState<AWeaponGameState>();
	if (!GameState)
	{
		OutError = TEXT("Current World does not have AWeaponGameState");
		return false;
	}

	FWeaponSnapshot MatchSnapshot;
	MatchSnapshot.MatchId = GetOrCreateActiveMatchId();
	MatchSnapshot.Revision = NextSnapshotRevision++;
	MatchSnapshot.MatchPhase = GameState->GetMatchPhase();
	MatchSnapshot.RemainingPhaseTime = GameState->GetRemainingPhaseTime();
	MatchSnapshot.MissionSeed = GameState->GetMissionSettings().Seed;
	MatchSnapshot.MissionDifficulty = GameState->GetMissionSettings().Difficulty;
	MatchSnapshot.ReinforcementPool = GameState->GetReinforcementPool();
	MatchSnapshot.MatchOutcome = GameState->GetMissionOutcome();
	if (const AWeaponGameMode* GameMode = World->GetAuthGameMode<AWeaponGameMode>())
	{
		GameMode->CaptureObjectiveSnapshot(MatchSnapshot.Objective);
		if (GameMode->WaveDirector)
		{
			GameMode->WaveDirector->CaptureSnapshotState(MatchSnapshot.WaveDirector);
		}
	}

	if (!CaptureRecoveryRoster(World, MatchSnapshot, OutError) ||
		!CapturePlayers(World, MatchSnapshot, OutError) ||
		!CaptureEnemies(World, MatchSnapshot, OutError) ||
		!CaptureGridMap(World, MatchSnapshot, OutError) ||
		!CaptureProjectiles(World, MatchSnapshot, OutError) ||
		!ValidateSnapshot(MatchSnapshot, GameState->GetMaxPlayers(), OutError))
	{
		return false;
	}

	OutSnapshot = MoveTemp(MatchSnapshot);
	return true;
}

bool UWeaponSnapshotCoordinator::ValidateSnapshot(
	const FWeaponSnapshot& Snapshot,
	const int32 MaxPlayers,
	FString& OutError)
{
	OutError.Reset();
	if (Snapshot.SchemaVersion != WeaponSnapshotVersion || Snapshot.Revision <= 0 ||
		!Snapshot.MatchId.IsValid() || !FMath::IsFinite(Snapshot.RemainingPhaseTime) ||
		Snapshot.RemainingPhaseTime < 0.0f)
	{
		OutError = TEXT("Snapshot version, identity, revision or phase time is invalid");
		return false;
	}
	if (MaxPlayers < 1 || MaxPlayers > 4 || Snapshot.RecoveryRoster.IsEmpty())
	{
		OutError = TEXT("Snapshot recovery roster or SaveGame capacity is invalid");
		return false;
	}

	if (Snapshot.MissionDifficulty < 1 || Snapshot.MissionDifficulty > 3 || Snapshot.MissionSeed < 0 ||
		!FMath::IsFinite(Snapshot.Objective.ExtractionRemainingSeconds) || Snapshot.Objective.ExtractionRemainingSeconds < 0.0f ||
		!FMath::IsFinite(Snapshot.Objective.ObjectiveChargeProgress) || !FMath::IsFinite(Snapshot.Objective.ExtractionHoldProgress) ||
		(Snapshot.WaveDirector.bArmed &&
			(Snapshot.WaveDirector.Difficulty < 1 || Snapshot.WaveDirector.Difficulty > 3)))
	{
		OutError = TEXT("Snapshot mission, objective or wave director state is invalid");
		return false;
	}
	{
		const FWeaponWaveDirectorSnapshot& Director = Snapshot.WaveDirector;
		const int32 RoomLimit = Director.CombatRooms.Num() + (Director.bHasDefenseWave ? 1 : 0);
		for (int32 Index = 0; Index < Director.SwarmCounts.Num(); ++Index)
		{
			const FWeaponSwarmRoomCount& Entry = Director.SwarmCounts[Index];
			if (Entry.Count <= 0 || !SwarmFindArchetypeTuning(Entry.ArchetypeRow) ||
				Entry.RoomIndex < 0 || Entry.RoomIndex >= RoomLimit)
			{
				OutError = FString::Printf(TEXT("Snapshot swarm count entry %d is invalid"), Index);
				return false;
			}
		}
	}
	if (Snapshot.ReinforcementPool < 0 || Snapshot.ReinforcementPool > 99 ||
		Snapshot.MatchOutcome > EWeaponMissionOutcome::Failed)
	{
		OutError = TEXT("Snapshot reinforcement pool or mission outcome is invalid");
		return false;
	}
	for (const FWeaponPlayerSnapshot& Player : Snapshot.Players)
	{
		if (Player.LifeState > EWeaponPlayerLifeState::Dead ||
			!FMath::IsFinite(Player.DownedRemainingSeconds) || Player.DownedRemainingSeconds < 0.0f)
		{
			OutError = TEXT("Snapshot player life state or downed timer is invalid");
			return false;
		}
		if (Player.bRespawnPending &&
			(!FMath::IsFinite(Player.RespawnRemainingSeconds) || Player.RespawnRemainingSeconds < 0.0f))
		{
			OutError = TEXT("Snapshot player respawn timer is invalid");
			return false;
		}
	}

	return ValidateRecoveryRoster(Snapshot, MaxPlayers, OutError) &&
		ValidatePlayers(Snapshot, OutError) &&
		ValidateEnemies(Snapshot, OutError) &&
		ValidateGridMap(Snapshot, OutError) &&
		ValidateProjectiles(Snapshot, OutError);
}

bool UWeaponSnapshotCoordinator::StageRestoreActors(
	const FWeaponSnapshot& Snapshot,
	FRestoreContext& Context,
	FString& OutError) const
{
	for (FRestoreContext::FPlayerTarget& Target : Context.Players)
	{
		const FWeaponPlayerSnapshot& Player = *Target.Snapshot;
		if (!Player.bHasPawn || Target.Character)
		{
			continue;
		}
		if (Target.bAttributionTombstone)
		{
			FActorSpawnParameters SpawnParameters;
			SpawnParameters.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
			Target.PlayerState = Context.World->SpawnActor<AWeaponPlayerState>(
				Context.PlayerStateClass,
				FTransform::Identity,
				SpawnParameters);
			if (!Target.PlayerState)
			{
				OutError = FString::Printf(TEXT("Failed to stage attribution PlayerState for %s"),
					*Player.PlayerId.ToDebugString());
				return false;
			}
			Target.bPlayerStateStaged = true;
			Target.PlayerState->SetUniqueId(Player.PlayerId);
			Target.PlayerState->SetPlayerName(Player.PlayerName);
			Target.PlayerState->SetTeamId(Player.TeamId);
			Context.GameState->RemovePlayerState(Target.PlayerState);
			Target.ASC = Target.PlayerState->GetAbilitySystemComponent();
		}
		if (!Target.PlayerState->SetProfessionLoadout(
			Player.ProfessionId, Player.PrimaryWeaponId, Player.SecondaryWeaponId))
		{
			OutError = FString::Printf(TEXT("Failed to stage profession loadout for player %s"),
				*Player.PlayerId.ToDebugString());
			return false;
		}

		const FTransform SpawnTransform(Player.Rotation, Player.Location);
		AWeaponCharacter* Character = Context.World->SpawnActorDeferred<AWeaponCharacter>(
			Target.PawnClass,
			SpawnTransform,
			Target.PlayerController,
			nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Character)
		{
			OutError = FString::Printf(TEXT("Failed to stage Character for player %s"),
				*Player.PlayerId.ToDebugString());
			return false;
		}

		Target.Character = Character;
		Target.bCharacterStaged = true;
		Character->SetActorEnableCollision(false);
		Character->SetActorTickEnabled(false);
		Character->FinishSpawning(SpawnTransform);
		Character->SetActorEnableCollision(false);
		Character->SetActorTickEnabled(false);
		if (Target.bAttributionTombstone)
		{
			Character->InitializeAttributionTombstone(Target.PlayerState);
			if (!Character->SetCharacterConfigRowNameAuthoritative(Player.CharacterConfigRowName))
			{
				OutError = FString::Printf(TEXT("Failed to stage Character config for player %s"),
					*Player.PlayerId.ToDebugString());
				return false;
			}
		}

		Target.Equipment = Character->FindComponentByClass<UWeaponEquipmentComponent>();
		if (!Character->GetCharacterMovement() || !Target.Equipment ||
			!CanRestoreAttributeValues(Target.ASC, Player.AttributeState.Values))
		{
			OutError = FString::Printf(TEXT("Staged Character for player %s is not Restore-compatible"),
				*Player.PlayerId.ToDebugString());
			return false;
		}
		Context.FinalPawnsByPlayerId.Add(Player.PlayerId, Character);
	}

	Context.StagedEnemies.Reserve(Snapshot.Enemies.Num());
	for (int32 Index = 0; Index < Snapshot.Enemies.Num(); ++Index)
	{
		const FWeaponEnemySnapshot& EnemySnap = Snapshot.Enemies[Index];
		const FTransform SpawnTransform(EnemySnap.Rotation, EnemySnap.Location);
		AEnemyCharacter* Enemy = Context.World->SpawnActorDeferred<AEnemyCharacter>(
			Context.EnemyClasses[Index],
			SpawnTransform,
			nullptr,
			nullptr,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Enemy)
		{
			OutError = FString::Printf(TEXT("Failed to stage enemy %d"), Index);
			return false;
		}

		Context.StagedEnemies.Add(Enemy);
		Enemy->SetActorEnableCollision(false);
		Enemy->SetActorTickEnabled(false);
		Enemy->SetEnemyConfigRowName(EnemySnap.EnemyConfigRowName);
		Enemy->FinishSpawning(SpawnTransform);
		Enemy->SetActorEnableCollision(false);
		Enemy->SetActorTickEnabled(false);
		if (!Enemy->GetCharacterMovement() ||
			!CanRestoreAttributeValues(Enemy->GetAbilitySystemComponent(), EnemySnap.AttributeState.Values))
		{
			OutError = FString::Printf(TEXT("Staged enemy %d is not Restore-compatible"), Index);
			return false;
		}
	}

	Context.StagedProjectiles.Reserve(Snapshot.Projectiles.Num() + Snapshot.Grenades.Num());
	for (int32 Index = 0; Index < Snapshot.Projectiles.Num(); ++Index)
	{
		const FWeaponProjectileSnapshot& SavedProjectile = Snapshot.Projectiles[Index];
		APawn* const* FoundInstigator = Context.FinalPawnsByPlayerId.Find(SavedProjectile.InstigatorPlayerId);
		if (!FoundInstigator || !*FoundInstigator)
		{
			OutError = FString::Printf(TEXT("Failed to resolve staged projectile %d instigator"), Index);
			return false;
		}

		const FTransform SpawnTransform(SavedProjectile.Rotation, SavedProjectile.Location);
		AWeaponProjectile* Projectile = Context.World->SpawnActorDeferred<AWeaponProjectile>(
			Context.ProjectileClasses[Index],
			FTransform::Identity,
			*FoundInstigator,
			*FoundInstigator,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Projectile)
		{
			OutError = FString::Printf(TEXT("Failed to stage projectile %d"), Index);
			return false;
		}

		Context.StagedProjectiles.Add(Projectile);
		Projectile->SetActorEnableCollision(false);
		Projectile->SetActorTickEnabled(false);
		Projectile->InitializeProjectile(
			Context.ProjectileDamageEffects[Index],
			SavedProjectile.DamageValue,
			*FoundInstigator,
			SavedProjectile.DamageTags,
			SavedProjectile.bPenetratesTerrain,
			SavedProjectile.ProjectileSpeed,
			SavedProjectile.DamageFalloffStart,
			SavedProjectile.DamageFalloffEnd,
			SavedProjectile.ArmorPenetration,
			SavedProjectile.StatusEffectId,
			SavedProjectile.InstigatorPlayerId);
		Projectile->InitFlightParams(
			SavedProjectile.RemainingLifeSeconds,
			SavedProjectile.ProjectileGravityScale,
			SavedProjectile.MaxHitTargets);
		Projectile->InitAreaDamageParams(
			SavedProjectile.AreaDamage,
			SavedProjectile.AreaRadius,
			SavedProjectile.AreaFalloff);
		Projectile->FinishSpawning(SpawnTransform);
		Projectile->SetActorEnableCollision(false);
		Projectile->SetActorTickEnabled(false);
		Projectile->RestoreSnapshotMotion(
			SavedProjectile.Velocity,
			SavedProjectile.RemainingLifeSeconds);
	}

	for (int32 Index = 0; Index < Snapshot.Grenades.Num(); ++Index)
	{
		const FWeaponGrenadeSnapshot& SavedGrenade = Snapshot.Grenades[Index];
		APawn* const* FoundInstigator = Context.FinalPawnsByPlayerId.Find(SavedGrenade.InstigatorPlayerId);
		if (!FoundInstigator || !*FoundInstigator)
		{
			OutError = FString::Printf(TEXT("Failed to resolve staged grenade %d instigator"), Index);
			return false;
		}

		const FTransform SpawnTransform(SavedGrenade.Rotation, SavedGrenade.Location);
		AGrenadeProjectile* Grenade = Context.World->SpawnActorDeferred<AGrenadeProjectile>(
			Context.GrenadeDataAssets[Index]->ProjectileClass,
			FTransform::Identity,
			*FoundInstigator,
			*FoundInstigator,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Grenade)
		{
			OutError = FString::Printf(TEXT("Failed to stage grenade %d"), Index);
			return false;
		}

		Context.StagedProjectiles.Add(Grenade);
		Grenade->SetActorEnableCollision(false);
		Grenade->SetActorTickEnabled(false);
		Grenade->InitFromGrenadeData(
			Context.GrenadeDataAssets[Index],
			*FoundInstigator,
			SavedGrenade.InstigatorPlayerId);
		Grenade->SetFuseTimeForSnapshotRestore(SavedGrenade.RemainingFuseTime);
		Grenade->FinishSpawning(SpawnTransform);
		Grenade->SetActorEnableCollision(false);
		Grenade->SetActorTickEnabled(false);
		Grenade->RestoreSnapshotMotion(SavedGrenade.Velocity, 0.0f);
	}
	return true;
}

void UWeaponSnapshotCoordinator::CleanupStagedActors(FRestoreContext& Context)
{
	for (AWeaponProjectile* Projectile : Context.StagedProjectiles)
	{
		if (Projectile)
		{
			Projectile->Destroy();
		}
	}
	Context.StagedProjectiles.Reset();

	for (AEnemyCharacter* Enemy : Context.StagedEnemies)
	{
		if (Enemy)
		{
			if (AController* Controller = Enemy->GetController())
			{
				Controller->Destroy();
			}
			Enemy->Destroy();
		}
	}
	Context.StagedEnemies.Reset();

	for (FRestoreContext::FPlayerTarget& Target : Context.Players)
	{
		if (Target.bCharacterStaged && Target.Character)
		{
			if (Target.bAttributionTombstone)
			{
				Target.Character->SetPlayerState(nullptr);
			}
			Target.Character->Destroy();
			Target.Character = nullptr;
			Target.Equipment = nullptr;
			Target.bCharacterStaged = false;
		}
		if (Target.bPlayerStateStaged && Target.PlayerState)
		{
			Context.GameState->RemovePlayerState(Target.PlayerState);
			Target.PlayerState->Destroy();
			Target.PlayerState = nullptr;
			Target.ASC = nullptr;
			Target.bPlayerStateStaged = false;
		}
	}
}

void UWeaponSnapshotCoordinator::ApplySnapshot(
	const UWeaponMatchSaveGame* SaveGame,
	FRestoreContext& Context)
{
	const FWeaponSnapshot& Snapshot = SaveGame->Snapshot;

	for (FRestoreContext::FPlayerTarget& Target : Context.Players)
	{
		const FWeaponPlayerSnapshot& Player = *Target.Snapshot;
		if (!Target.bAttributionTombstone)
		{
			Target.PlayerState->SetPlayerName(Player.PlayerName);
			Target.PlayerState->SetPlayerReady(Player.bIsReady);
			Target.PlayerState->SetTeamId(Player.TeamId);
			if (!Target.PlayerState->SetProfessionLoadout(
				Player.ProfessionId, Player.PrimaryWeaponId, Player.SecondaryWeaponId))
			{
				UE_LOG(LogTemp, Fatal, TEXT("Validated profession loadout became invalid during Snapshot Apply"));
			}
			if (Target.Character)
			{
				Target.Character->RefreshProfessionLoadout();
			}
		}
		if (Target.bAttributionTombstone)
		{
			RestoreAttributeValues(Target.ASC, Player.AttributeState.Values);
			Context.GameMode->AdoptRecoveredAttributionTombstone(
				Player.PlayerId,
				Target.Character,
				Target.PlayerState);
			continue;
		}
		SnapshotAppliedPlayerIds.Add(Player.PlayerId);
		if (!Player.bHasPawn)
		{
			if (Target.ExistingPawn)
			{
				if (UWeaponEquipmentComponent* Equipment =
					Target.ExistingPawn->FindComponentByClass<UWeaponEquipmentComponent>())
				{
					Equipment->UnequipWeapon();
				}
				if (Target.PlayerController && Target.PlayerController->GetPawn() == Target.ExistingPawn)
				{
					Target.PlayerController->UnPossess();
				}
				Target.ExistingPawn->Destroy();
			}
			Target.ASC->InitAbilityActorInfo(Target.PlayerState, nullptr);
			RestoreAttributeValues(Target.ASC, Player.AttributeState.Values);
			// v10:InGame 中无 Pawn = 死亡等待复活。恢复世界没有原 Timer,在此重建(屏障内暂停)。
			ApplyPlayerLifeState(Context.GameMode, Target.PlayerController, Target.PlayerState,
				nullptr, Player, Snapshot.MatchPhase);
			continue;
		}

		if (Target.bCharacterStaged)
		{
			if (Target.ExistingPawn)
			{
				if (UWeaponEquipmentComponent* ExistingEquipment =
					Target.ExistingPawn->FindComponentByClass<UWeaponEquipmentComponent>())
				{
					ExistingEquipment->UnequipWeapon();
				}
				if (Target.PlayerController->GetPawn() == Target.ExistingPawn)
				{
					Target.PlayerController->UnPossess();
				}
			}

			Target.PlayerController->Possess(Target.Character);
			if (Target.PlayerController->GetPawn() != Target.Character)
			{
				UE_LOG(LogTemp, Fatal, TEXT("Validated Character possession failed during Snapshot Apply"));
			}
			if (Target.ExistingPawn)
			{
				Target.ExistingPawn->Destroy();
			}
		}
		if (!Target.Character->SetCharacterConfigRowNameAuthoritative(Player.CharacterConfigRowName))
		{
			UE_LOG(LogTemp, Fatal, TEXT("Validated Character config became invalid during Snapshot Apply"));
		}

		Target.Character->SetActorLocation(Player.Location, false, nullptr, ETeleportType::TeleportPhysics);
		Target.Character->SetAimDirectionAuthoritative(Player.Rotation.Vector());
		Target.Character->GetCharacterMovement()->Velocity = Player.Velocity;
		if (!Target.Equipment->RestoreSnapshotState(Target.EquipmentSnapshot) ||
			!Target.Character->RestoreEquipmentSlot(Player.CurrentEquipmentSlot))
		{
			UE_LOG(LogTemp, Fatal, TEXT("Validated equipment state became invalid during Snapshot Apply"));
		}
		RestoreAttributeValues(Target.ASC, Player.AttributeState.Values);
		if (Target.bCharacterStaged)
		{
			Target.Character->SetActorTickEnabled(true);
			Target.Character->SetActorEnableCollision(true);
			Target.Character->ForceNetUpdate();
		}

		// v12:显式写回生存状态(默认 Up 不能泄漏到死亡/倒地玩家),并补齐服务端 GAS Tag。
		ApplyPlayerLifeState(Context.GameMode, Target.PlayerController, Target.PlayerState,
			Target.Character, Player, Snapshot.MatchPhase);
	}

	for (int32 Index = 0; Index < Context.StagedEnemies.Num(); ++Index)
	{
		AEnemyCharacter* Enemy = Context.StagedEnemies[Index];
		const FWeaponEnemySnapshot& EnemySnap = Snapshot.Enemies[Index];
		Enemy->GetCharacterMovement()->Velocity = EnemySnap.Velocity;
		RestoreAttributeValues(Enemy->GetAbilitySystemComponent(), EnemySnap.AttributeState.Values);
		RestoreEnemyStatusEffects(Enemy, EnemySnap.StatusEffects);
	}
	for (AEnemyCharacter* Enemy : Context.ExistingEnemies)
	{
		Enemy->Destroy();
	}
	for (AEnemyCharacter* Enemy : Context.StagedEnemies)
	{
		Enemy->SetActorTickEnabled(true);
		Enemy->SetActorEnableCollision(true);
		Enemy->ForceNetUpdate();
	}

	for (AWeaponProjectile* Projectile : Context.ExistingProjectiles)
	{
		Projectile->Destroy();
	}
	for (AWeaponProjectile* Projectile : Context.StagedProjectiles)
	{
		Projectile->SetGameplayRecoveryBlocked(false);
		Projectile->SetActorTickEnabled(true);
		Projectile->SetActorEnableCollision(true);
		Projectile->SetGameplayRecoveryBlocked(true);
		Projectile->ForceNetUpdate();
	}

	const int32 PreviousMaxPlayers = Context.GameState->GetMaxPlayers();
	if (SaveGame->MaxPlayers >= PreviousMaxPlayers &&
		!Context.GameState->SetMaxPlayersAuthority(SaveGame->MaxPlayers))
	{
		UE_LOG(LogTemp, Fatal, TEXT("Validated player capacity became invalid during Snapshot Apply"));
	}

	for (const FRestoreContext::FPlayerTarget& Target : Context.Players)
	{
		const FWeaponPlayerSnapshot& Player = *Target.Snapshot;
		if (Target.bAttributionTombstone)
		{
			continue;
		}
		Target.PlayerState->SetPlayerName(Player.PlayerName);
		Target.PlayerState->SetPlayerReady(Player.bIsReady);
		Target.PlayerState->SetTeamId(Player.TeamId);
	}
	for (const FWeaponRecoveryRosterSnapshot& RosterEntry : Snapshot.RecoveryRoster)
	{
		if (AWeaponPlayerState* const* ConnectedPlayerState =
			Context.ConnectedPlayersById.Find(RosterEntry.PlayerId))
		{
			(*ConnectedPlayerState)->SetPlayerSlotIndex(RosterEntry.OriginalSlot);
		}
	}

	if (SaveGame->MaxPlayers < PreviousMaxPlayers &&
		!Context.GameState->SetMaxPlayersAuthority(SaveGame->MaxPlayers))
	{
		UE_LOG(LogTemp, Fatal, TEXT("Validated player capacity became invalid during Snapshot Apply"));
	}
	if (!Context.GameMode->SetRecoveryRoster(Snapshot.RecoveryRoster, Snapshot.RosterRevision))
	{
		UE_LOG(LogTemp, Fatal, TEXT("Validated recovery roster became invalid during Snapshot Apply"));
	}

	Context.GameMode->ApplyRecoveredMatchPhase(Snapshot.MatchPhase, Snapshot.RemainingPhaseTime);

	// v12:增援池与任务结局(屏障内写回;任务失败判定在屏障释放后由 GameMode 重检)。
	Context.GameState->RestoreReinforcementPoolForRecovery(Snapshot.ReinforcementPool);
	Context.GameState->SetMissionOutcomeAuthority(Snapshot.MatchOutcome);

	// v10:恢复世界不解析 travel options,任务设置以快照为准。
	FWeaponMissionSettings RestoredMission;
	RestoredMission.Seed = Snapshot.MissionSeed;
	RestoredMission.Difficulty = Snapshot.MissionDifficulty;
	Context.GameState->RestoreMissionSettingsForRecovery(RestoredMission);
	// GridMap Recipe 与敌人已在前面恢复:目标/撤离 Actor 依赖 marker,导演依赖 Recipe 与在场敌人。
	Context.GameMode->RestoreObjectiveSnapshot(Snapshot.Objective);
	if (Context.GameMode->WaveDirector)
	{
		Context.GameMode->WaveDirector->RestoreSnapshotState(Context.GridMap, Snapshot.WaveDirector);
	}
	ActiveMatchId = Snapshot.MatchId;
	NextSnapshotRevision = Snapshot.Revision + 1;
}

bool UWeaponSnapshotCoordinator::CapturePlayers(
	const UWorld* World,
	FWeaponSnapshot& Snapshot,
	FString& OutError)
{
	const AWeaponGameState* GameState = World->GetGameState<AWeaponGameState>();
	const AWeaponGameMode* GameMode = World->GetAuthGameMode<AWeaponGameMode>();
	if (!GameState || !GameMode)
	{
		OutError = TEXT("Snapshot player Capture requires authoritative match owners");
		return false;
	}

	TSet<FUniqueNetIdRepl> CapturedPlayerIds;
	TMap<FUniqueNetIdRepl, AWeaponCharacter*> TombstonePawns;
	TMap<FUniqueNetIdRepl, AWeaponPlayerState*> TombstonePlayerStates;
	GameMode->GetAttributionTombstoneActors(TombstonePawns, TombstonePlayerStates);
	auto CapturePlayer = [&Snapshot, &OutError, &CapturedPlayerIds, GameState, GameMode](
		const AWeaponPlayerState* PlayerState,
		const AWeaponCharacter* Character) -> bool
	{
		if (!PlayerState || !PlayerState->GetUniqueId().IsValid() ||
			CapturedPlayerIds.Contains(PlayerState->GetUniqueId()))
		{
			return false;
		}

		FWeaponPlayerSnapshot& Player = Snapshot.Players.AddDefaulted_GetRef();
		Player.PlayerId = ResolvePlayerId(PlayerState);
		Player.PlayerName = PlayerState->GetPlayerName();
		Player.SlotIndex = PlayerState->GetPlayerSlotIndex();
		Player.bIsReady = PlayerState->GetIsPlayerReady();
		Player.TeamId = PlayerState->GetTeamId();
		Player.ProfessionId = PlayerState->GetProfessionId();
		Player.PrimaryWeaponId = PlayerState->GetPrimaryWeaponId();
		Player.SecondaryWeaponId = PlayerState->GetSecondaryWeaponId();
		Player.AttributeState.Values = CaptureAttributeValues(PlayerState->GetAbilitySystemComponent());
		CapturedPlayerIds.Add(Player.PlayerId);
		// v10:死亡等待复活的剩余秒数(RespawnServerTime 为复活 Timer 的复制投影)
		if (const float RespawnServerTime = PlayerState->GetRespawnServerTime(); RespawnServerTime > 0.0f)
		{
			Player.bRespawnPending = true;
			Player.RespawnRemainingSeconds = FMath::Max(0.0f, RespawnServerTime - GameState->GetServerWorldTimeSeconds());
		}
		// v12:生存状态与流血剩余秒数(DownedServerTime 为流血 Timer 的复制投影,屏障内由 Release 重新锚定)
		Player.LifeState = PlayerState->GetLifeState();
		if (Player.LifeState == EWeaponPlayerLifeState::Downed)
		{
			// 优先读 Timer 本身:屏障内 Timer 暂停,而复制投影会随时间下降,会低估剩余时间。
			// 取两者较大值:该玩家没有流血 Timer(如离线墓碑)时自然回退到复制投影。
			const float Projected = FMath::Max(
				0.0f, PlayerState->GetDownedServerTime() - GameState->GetServerWorldTimeSeconds());
			const float Exact = GameMode->GetBleedOutRemaining(PlayerState->GetOwningController());
			Player.DownedRemainingSeconds = FMath::Max(Projected, Exact);
		}

		if (!Character)
		{
			return true;
		}

		Player.bHasPawn = true;
		Player.CharacterConfigRowName = Character->GetCharacterConfigRowName();
		Player.CurrentEquipmentSlot = Character->GetCurrentEquipmentSlot();
		Player.Location = Character->GetActorLocation();
		Player.Rotation = FRotator(0.0f, Character->GetActorRotation().Yaw, 0.0f);
		Player.Velocity = Character->GetVelocity();

		const UWeaponEquipmentComponent* Equipment = Character->FindComponentByClass<UWeaponEquipmentComponent>();
		if (!Equipment)
		{
			OutError = FString::Printf(TEXT("Player %s does not have WeaponEquipmentComponent"),
				*Player.PlayerName);
			return false;
		}
		FWeaponEquipmentSnapshotState EquipmentSnapshot;
		Equipment->CaptureSnapshotState(EquipmentSnapshot);
		Player.CurrentWeaponIndex = EquipmentSnapshot.CurrentWeaponIndex;
		Player.bWasReloading = EquipmentSnapshot.bWasReloading;
		for (UWeaponDataAsset* Weapon : EquipmentSnapshot.WeaponInventory)
		{
			Player.WeaponInventory.Add(FSoftObjectPath(Weapon));
		}
		for (const FWeaponAmmoState& Ammo : EquipmentSnapshot.WeaponAmmoStates)
		{
			FWeaponAmmoSnapshot& SavedAmmo = Player.WeaponAmmoStates.AddDefaulted_GetRef();
			SavedAmmo.MagazineAmmo = Ammo.MagazineAmmo;
			SavedAmmo.ReserveAmmo = Ammo.ReserveAmmo;
		}
		return true;
	};

	for (APlayerState* BasePlayerState : GameState->PlayerArray)
	{
		const AWeaponPlayerState* PlayerState = Cast<AWeaponPlayerState>(BasePlayerState);
		const AWeaponCharacter* Character = PlayerState
			? Cast<AWeaponCharacter>(PlayerState->GetPawn())
			: nullptr;
		if (PlayerState && !Character && TombstonePawns.Contains(PlayerState->GetUniqueId()))
		{
			continue;
		}
		if (!PlayerState ||
			!CapturePlayer(PlayerState, Character))
		{
			OutError = OutError.IsEmpty()
				? TEXT("Connected player state is invalid or duplicated during Snapshot Capture")
				: OutError;
			return false;
		}
	}

	for (const auto& [PlayerId, TombstonePawn] : TombstonePawns)
	{
		if (CapturedPlayerIds.Contains(PlayerId))
		{
			continue;
		}
		AWeaponPlayerState* const* TombstonePlayerState = TombstonePlayerStates.Find(PlayerId);
		if (!TombstonePlayerState || !CapturePlayer(*TombstonePlayerState, TombstonePawn))
		{
			OutError = OutError.IsEmpty()
				? TEXT("Attribution tombstone is incomplete during Snapshot Capture")
				: OutError;
			return false;
		}
	}

	return true;
}

bool UWeaponSnapshotCoordinator::CaptureRecoveryRoster(
	const UWorld* World,
	FWeaponSnapshot& Snapshot,
	FString& OutError)
{
	const AWeaponGameMode* GameMode = World ? World->GetAuthGameMode<AWeaponGameMode>() : nullptr;
	if (!GameMode)
	{
		OutError = TEXT("Snapshot roster Capture requires AWeaponGameMode authority");
		return false;
	}

	Snapshot.RosterRevision = GameMode->GetRosterRevision();
	const double CurrentServerTime = World->GetTimeSeconds();
	for (const auto& [PlayerId, RuntimeEntry] : GameMode->GetRecoveryRoster())
	{
		FWeaponRecoveryRosterSnapshot& SavedEntry = Snapshot.RecoveryRoster.AddDefaulted_GetRef();
		SavedEntry.PlayerId = PlayerId;
		SavedEntry.OriginalSlot = RuntimeEntry.OriginalSlot;
		SavedEntry.Status = RuntimeEntry.Status;
		if (RuntimeEntry.Status == EWeaponRecoveryPlayerStatus::Pending)
		{
			SavedEntry.RemainingGraceSeconds = static_cast<float>(
				FMath::Max(0.0, RuntimeEntry.PendingDeadlineServerTime - CurrentServerTime));
		}
	}
	return true;
}

bool UWeaponSnapshotCoordinator::ValidateRecoveryRoster(
	const FWeaponSnapshot& Snapshot,
	const int32 MaxPlayers,
	FString& OutError)
{
	if (Snapshot.RosterRevision == 0 || Snapshot.RosterRevision == MAX_uint64)
	{
		OutError = TEXT("Snapshot recovery roster revision is invalid");
		return false;
	}

	TSet<FUniqueNetIdRepl> PlayerIds;
	TSet<int32> ReservedSlots;
	for (const FWeaponRecoveryRosterSnapshot& Entry : Snapshot.RecoveryRoster)
	{
		const bool bKnownStatus = Entry.Status == EWeaponRecoveryPlayerStatus::Pending ||
			Entry.Status == EWeaponRecoveryPlayerStatus::Ready ||
			Entry.Status == EWeaponRecoveryPlayerStatus::Departed;
		if (!Entry.PlayerId.IsValid() || PlayerIds.Contains(Entry.PlayerId) || !bKnownStatus ||
			Entry.OriginalSlot < 0 || Entry.OriginalSlot >= MaxPlayers ||
			!FMath::IsFinite(Entry.RemainingGraceSeconds) || Entry.RemainingGraceSeconds < 0.0f ||
			(Entry.Status != EWeaponRecoveryPlayerStatus::Pending &&
			 Entry.RemainingGraceSeconds != 0.0f))
		{
			OutError = TEXT("Snapshot recovery roster entry is invalid or duplicated");
			return false;
		}
		if (Entry.Status != EWeaponRecoveryPlayerStatus::Departed)
		{
			if (ReservedSlots.Contains(Entry.OriginalSlot))
			{
				OutError = TEXT("Snapshot active recovery roster has duplicated Slot reservations");
				return false;
			}
			ReservedSlots.Add(Entry.OriginalSlot);
		}
		PlayerIds.Add(Entry.PlayerId);
	}
	return true;
}

bool UWeaponSnapshotCoordinator::ValidatePlayers(
	const FWeaponSnapshot& Snapshot,
	FString& OutError)
{
	TSet<FUniqueNetIdRepl> PlayerIds;
	TMap<FUniqueNetIdRepl, int32> RosterSlots;
	for (const FWeaponRecoveryRosterSnapshot& Entry : Snapshot.RecoveryRoster)
	{
		RosterSlots.Add(Entry.PlayerId, Entry.OriginalSlot);
	}
	for (const FWeaponPlayerSnapshot& Player : Snapshot.Players)
	{
		const int32* RosterSlot = RosterSlots.Find(Player.PlayerId);
		if (!Player.PlayerId.IsValid() || Player.PlayerName.IsEmpty() ||
			PlayerIds.Contains(Player.PlayerId) || !RosterSlot || *RosterSlot != Player.SlotIndex ||
			!ValidateAttributeValues(Player.AttributeState.Values))
		{
			OutError = TEXT("Snapshot player has invalid or duplicated identity, slot or attributes");
			return false;
		}

		const bool bHasAnyProfessionLoadout = !Player.ProfessionId.IsNone() || !Player.PrimaryWeaponId.IsNone() || !Player.SecondaryWeaponId.IsNone();
		if ((Player.bHasPawn || bHasAnyProfessionLoadout) &&
			(Player.ProfessionId.IsNone() || Player.PrimaryWeaponId.IsNone() || Player.SecondaryWeaponId.IsNone()))
		{
			OutError = TEXT("Snapshot player has invalid profession loadout");
			return false;
		}
		if (Player.bHasPawn)
		{
			if (Player.CurrentEquipmentSlot > 3 || Player.CharacterConfigRowName.IsNone() || !IsFiniteVector(Player.Location) ||
				!IsFiniteRotator(Player.Rotation) || !IsFiniteVector(Player.Velocity))
			{
				OutError = TEXT("Snapshot has invalid player Pawn state");
				return false;
			}

			if (Player.CurrentWeaponIndex < INDEX_NONE)
			{
				OutError = TEXT("Snapshot has invalid current weapon index");
				return false;
			}
			if (Player.WeaponInventory.Num() != Player.WeaponAmmoStates.Num() ||
				(Player.CurrentWeaponIndex != INDEX_NONE && !Player.WeaponInventory.IsValidIndex(Player.CurrentWeaponIndex)))
			{
				OutError = TEXT("Snapshot has inconsistent weapon inventory state");
				return false;
			}
			for (int32 Index = 0; Index < Player.WeaponInventory.Num(); ++Index)
			{
				if (!Player.WeaponInventory[Index].IsValid() || Player.WeaponAmmoStates[Index].MagazineAmmo < 0 || Player.WeaponAmmoStates[Index].ReserveAmmo < 0)
				{
					OutError = TEXT("Snapshot has invalid weapon identity or ammo state");
					return false;
				}
			}
		}
		else if (Player.CurrentWeaponIndex != INDEX_NONE)
		{
			OutError = TEXT("Snapshot player without Pawn cannot select a Pawn weapon");
			return false;
		}

		PlayerIds.Add(Player.PlayerId);
	}

	return true;
}

bool UWeaponSnapshotCoordinator::CaptureEnemies(
	const UWorld* World,
	FWeaponSnapshot& Snapshot,
	FString& OutError)
{
	for (TActorIterator<AEnemyCharacter> It(World); It; ++It)
	{
		const AEnemyCharacter* Enemy = *It;
		auto& [EnemyClass, Location, Rotation, Velocity, EnemyConfigRowName, AttributeState, StatusEffects] = Snapshot.Enemies.AddDefaulted_GetRef();
		EnemyClass = FSoftClassPath(Enemy->GetClass());
		Location = Enemy->GetActorLocation();
		Rotation = FRotator(0.0f, Enemy->GetActorRotation().Yaw, 0.0f);
		Velocity = Enemy->GetVelocity();
		EnemyConfigRowName = Enemy->GetEnemyConfigRowName();
		AttributeState.Values = CaptureAttributeValues(Enemy->GetAbilitySystemComponent());
		if (const UStatusEffectComponent* StatusComponent = Enemy->FindComponentByClass<UStatusEffectComponent>())
		{
			TArray<FStatusEffectSnapshotEntry> Entries;
			StatusComponent->CaptureSnapshotState(Entries);
			for (const FStatusEffectSnapshotEntry& Entry : Entries)
			{
				FWeaponStatusEffectSnapshot& Saved = StatusEffects.AddDefaulted_GetRef();
				Saved.StatusEffectId = Entry.StatusEffectId;
				Saved.StackCount = Entry.StackCount;
				Saved.RemainingDuration = Entry.RemainingDuration;
				Saved.PendingHits = Entry.PendingHits;
				Saved.DamageEffectClass = FSoftClassPath(Entry.DamageEffectClass.Get());
			}
		}
	}
	return true;
}

bool UWeaponSnapshotCoordinator::ValidateEnemies(const FWeaponSnapshot& Snapshot, FString& OutError)
{
	for (const auto& [EnemyClass, Location, Rotation, Velocity, EnemyConfigRowName, AttributeState, StatusEffects] : Snapshot.Enemies)
	{
		if (!EnemyClass.IsValid() || EnemyConfigRowName.IsNone() ||
			!IsFiniteVector(Location) || !IsFiniteRotator(Rotation) ||
			!IsFiniteVector(Velocity) || !ValidateAttributeValues(AttributeState.Values))
		{
			OutError = TEXT("Snapshot has invalid enemy class, config, movement or attributes");
			return false;
		}
		for (const FWeaponStatusEffectSnapshot& Status : StatusEffects)
		{
			if (Status.StatusEffectId.IsNone() || Status.StackCount <= 0 || !Status.DamageEffectClass.IsValid() ||
				!FMath::IsFinite(Status.RemainingDuration) || Status.RemainingDuration < 0.0f || Status.PendingHits < 0)
			{
				OutError = TEXT("Snapshot has an invalid enemy status effect");
				return false;
			}
		}
	}

	return true;
}

bool UWeaponSnapshotCoordinator::CaptureGridMap(
	const UWorld* World,
	FWeaponSnapshot& Snapshot,
	FString& OutError)
{
	const AGridMapManager* GridMap = nullptr;
	for (TActorIterator<AGridMapManager> It(World); It; ++It)
	{
		if (GridMap)
		{
			OutError = TEXT("Snapshot currently supports exactly one GridMapManager");
			return false;
		}
		GridMap = *It;
	}

	if (!GridMap)
	{
		OutError = TEXT("Snapshot requires one GridMapManager");
		return false;
	}
	return GridMap->CaptureRecoveryState(Snapshot.GridMap, OutError);
}

bool UWeaponSnapshotCoordinator::ValidateGridMap(
	const FWeaponSnapshot& Snapshot,
	FString& OutError)
{
	return AGridMapManager::ValidateRecoveryState(Snapshot.GridMap, OutError);
}

bool UWeaponSnapshotCoordinator::CaptureProjectiles(
	const UWorld* World,
	FWeaponSnapshot& Snapshot,
	FString& OutError)
{
	for (TActorIterator<AWeaponProjectile> It(World); It; ++It)
	{
		const AWeaponProjectile* Projectile = *It;
		if (Projectile->IsActorBeingDestroyed())
		{
			continue;
		}

		const FWeaponProjectileInitializationState& Initialization = Projectile->GetInitializationState();
		const FUniqueNetIdRepl InstigatorPlayerId = Initialization.InstigatorPlayerId;
		if (!Initialization.bInitialized || !InstigatorPlayerId.IsValid())
		{
			OutError = FString::Printf(TEXT("Projectile %s has no restorable initialization or player instigator"),
				*GetNameSafe(Projectile));
			return false;
		}

		if (const AGrenadeProjectile* Grenade = Cast<AGrenadeProjectile>(Projectile))
		{
			if (Grenade->HasExploded())
			{
				continue;
			}

			FWeaponGrenadeSnapshot& SavedGrenade = Snapshot.Grenades.AddDefaulted_GetRef();
			SavedGrenade.GrenadeDataAsset = Grenade->GetGrenadeDataPath();
			SavedGrenade.InstigatorPlayerId = InstigatorPlayerId;
			SavedGrenade.Location = Grenade->GetActorLocation();
			SavedGrenade.Rotation = Grenade->GetActorRotation();
			SavedGrenade.Velocity = Grenade->GetProjectileVelocity();
			SavedGrenade.RemainingFuseTime = Grenade->GetRemainingFuseTime();
			continue;
		}

		FWeaponProjectileSnapshot& SavedProjectile = Snapshot.Projectiles.AddDefaulted_GetRef();
		SavedProjectile.ProjectileClass = FSoftClassPath(Projectile->GetClass());
		SavedProjectile.DamageEffectClass = FSoftClassPath(Initialization.DamageEffectClass.Get());
		SavedProjectile.InstigatorPlayerId = InstigatorPlayerId;
		SavedProjectile.Location = Projectile->GetActorLocation();
		SavedProjectile.Rotation = Projectile->GetActorRotation();
		SavedProjectile.Velocity = Projectile->GetProjectileVelocity();
		SavedProjectile.DamageTags = Initialization.DamageTags;
		SavedProjectile.StatusEffectId = Initialization.StatusEffectId;
		SavedProjectile.DamageValue = Initialization.DamageValue;
		SavedProjectile.ProjectileSpeed = Initialization.ProjectileSpeed;
		SavedProjectile.DamageFalloffStart = Initialization.DamageFalloffStart;
		SavedProjectile.DamageFalloffEnd = Initialization.DamageFalloffEnd;
		SavedProjectile.ArmorPenetration = Initialization.ArmorPenetration;
		SavedProjectile.AreaDamage = Initialization.AreaDamage;
		SavedProjectile.AreaRadius = Initialization.AreaRadius;
		SavedProjectile.AreaFalloff = Initialization.AreaFalloff;
		SavedProjectile.ProjectileGravityScale = Initialization.ProjectileGravityScale;
		SavedProjectile.MaxHitTargets = Initialization.MaxHitTargets;
		SavedProjectile.RemainingLifeSeconds = Projectile->GetLifeSpan();
		SavedProjectile.bPenetratesTerrain = Initialization.bPenetratesTerrain;
	}
	return true;
}

bool UWeaponSnapshotCoordinator::ValidateProjectiles(
	const FWeaponSnapshot& Snapshot,
	FString& OutError)
{
	TSet<FUniqueNetIdRepl> PlayersWithPawns;
	for (const FWeaponPlayerSnapshot& Player : Snapshot.Players)
	{
		if (Player.bHasPawn)
		{
			PlayersWithPawns.Add(Player.PlayerId);
		}
	}

	for (const FWeaponProjectileSnapshot& Projectile : Snapshot.Projectiles)
	{
		if (!Projectile.ProjectileClass.IsValid() || !Projectile.DamageEffectClass.IsValid() ||
			!PlayersWithPawns.Contains(Projectile.InstigatorPlayerId) ||
			!IsFiniteVector(Projectile.Location) || !IsFiniteRotator(Projectile.Rotation) ||
			!IsFiniteVector(Projectile.Velocity) || !FMath::IsFinite(Projectile.DamageValue) ||
			Projectile.DamageValue < 0.0f || !FMath::IsFinite(Projectile.ProjectileSpeed) ||
			Projectile.ProjectileSpeed <= 0.0f || !FMath::IsFinite(Projectile.DamageFalloffStart) ||
			Projectile.DamageFalloffStart < 0.0f || !FMath::IsFinite(Projectile.DamageFalloffEnd) ||
			Projectile.DamageFalloffEnd < Projectile.DamageFalloffStart || !FMath::IsFinite(Projectile.RemainingLifeSeconds) ||
			Projectile.RemainingLifeSeconds <= 0.0f || !FMath::IsFinite(Projectile.AreaDamage) || Projectile.AreaDamage < 0.0f ||
			!FMath::IsFinite(Projectile.AreaRadius) || Projectile.AreaRadius < 0.0f ||
			!FMath::IsFinite(Projectile.AreaFalloff) || Projectile.AreaFalloff < 0.0f || Projectile.AreaFalloff > 1.0f ||
			!FMath::IsFinite(Projectile.ProjectileGravityScale) || Projectile.MaxHitTargets < 1)
		{
			OutError = TEXT("Snapshot has invalid active projectile state");
			return false;
		}
	}

	for (const FWeaponGrenadeSnapshot& Grenade : Snapshot.Grenades)
	{
		if (!Grenade.GrenadeDataAsset.IsValid() ||
			!PlayersWithPawns.Contains(Grenade.InstigatorPlayerId) ||
			!IsFiniteVector(Grenade.Location) || !IsFiniteRotator(Grenade.Rotation) ||
			!IsFiniteVector(Grenade.Velocity) || !FMath::IsFinite(Grenade.RemainingFuseTime) ||
			Grenade.RemainingFuseTime <= 0.0f)
		{
			OutError = TEXT("Snapshot has invalid active grenade state");
			return false;
		}
	}
	return true;
}

TMap<FGameplayTag, float> UWeaponSnapshotCoordinator::CaptureAttributeValues(
	const UAbilitySystemComponent* ASC)
{
	TMap<FGameplayTag, float> Result;
	if (!ASC)
	{
		return Result;
	}
	for (const auto& [Tag, Attribute] : UAttributeModifierManager::GetAttributeTagMap())
	{
		if (ASC->HasAttributeSetForAttribute(Attribute))
		{
			// 读 Base 与 Restore 写 Base 对称:Current 含 GE 修饰器(增益/叠层),写回 Base 会把修饰器重复计入。
			Result.Add(Tag, ASC->GetNumericAttributeBase(Attribute));
		}
	}
	return Result;
}

bool UWeaponSnapshotCoordinator::ValidateAttributeValues(
	const TMap<FGameplayTag, float>& Values)
{
	const TMap<FGameplayTag, FGameplayAttribute>& AttributeMap = UAttributeModifierManager::GetAttributeTagMap();
	if (Values.IsEmpty())
	{
		return false;
	}

	for (const TPair<FGameplayTag, float>& Pair : Values)
	{
		const FGameplayAttribute* Attribute = AttributeMap.Find(Pair.Key);
		if (!Pair.Key.IsValid() || !Attribute || !Attribute->IsValid() || !FMath::IsFinite(Pair.Value))
		{
			return false;
		}
	}
	return true;
}

bool UWeaponSnapshotCoordinator::CanRestoreAttributeValues(
	const UAbilitySystemComponent* ASC,
	const TMap<FGameplayTag, float>& Values)
{
	if (!ASC || !ValidateAttributeValues(Values))
	{
		return false;
	}

	const TMap<FGameplayTag, FGameplayAttribute>& AttributeMap =
		UAttributeModifierManager::GetAttributeTagMap();
	for (const TPair<FGameplayTag, float>& Pair : Values)
	{
		const FGameplayAttribute* Attribute = AttributeMap.Find(Pair.Key);
		if (!Attribute || !ASC->HasAttributeSetForAttribute(*Attribute))
		{
			return false;
		}
	}
	return true;
}

void UWeaponSnapshotCoordinator::RestoreAttributeValues(
	UAbilitySystemComponent* ASC,
	const TMap<FGameplayTag, float>& Values)
{
	check(CanRestoreAttributeValues(ASC, Values));

	const FGameplayTag MaximumTags[] = {
		TAG_Attr_MaxHealth,
		TAG_Attr_MaxShield,
		TAG_Attr_MaxEnergy,
		TAG_Attr_MaxStamina,
	};
	for (const FGameplayTag& Tag : MaximumTags)
	{
		const float* Value = Values.Find(Tag);
		const FGameplayAttribute* Attribute = UAttributeModifierManager::GetAttributeTagMap().Find(Tag);
		if (Value)
		{
			ASC->SetNumericAttributeBase(*Attribute, *Value);
		}
	}

	for (const auto& [Tag, Value] : Values)
	{
		if (Tag == TAG_Attr_MaxHealth || Tag == TAG_Attr_MaxShield ||
			Tag == TAG_Attr_MaxEnergy || Tag == TAG_Attr_MaxStamina)
		{
			continue;
		}
		const FGameplayAttribute* Attribute = UAttributeModifierManager::GetAttributeTagMap().Find(Tag);
		ASC->SetNumericAttributeBase(*Attribute, Value);
	}
}

void UWeaponSnapshotCoordinator::RestoreEnemyStatusEffects(
	AEnemyCharacter* Enemy,
	const TArray<FWeaponStatusEffectSnapshot>& StatusEffects)
{
	UStatusEffectComponent* StatusComponent = Enemy ? Enemy->FindComponentByClass<UStatusEffectComponent>() : nullptr;
	for (const FWeaponStatusEffectSnapshot& Saved : StatusEffects)
	{
		FStatusEffectSnapshotEntry Entry;
		Entry.StatusEffectId = Saved.StatusEffectId;
		Entry.StackCount = Saved.StackCount;
		Entry.RemainingDuration = Saved.RemainingDuration;
		Entry.PendingHits = Saved.PendingHits;
		Entry.DamageEffectClass = Saved.DamageEffectClass.TryLoadClass<UGameplayEffect>();
		if (!StatusComponent || !StatusComponent->RestoreSnapshotEntry(Entry))
		{
			UE_LOG(LogLetsDeepRock, Warning, TEXT("Snapshot status effect %s on %s could not be restored; skipped"),
				*Saved.StatusEffectId.ToString(), *GetNameSafe(Enemy));
		}
	}
}

FUniqueNetIdRepl UWeaponSnapshotCoordinator::ResolvePlayerId(const APlayerState* PlayerState)
{
	return PlayerState ? PlayerState->GetUniqueId() : FUniqueNetIdRepl();
}

bool UWeaponSnapshotCoordinator::IsFiniteVector(const FVector& Value)
{
	return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
}

bool UWeaponSnapshotCoordinator::IsFiniteRotator(const FRotator& Value)
{
	return FMath::IsFinite(Value.Pitch) && FMath::IsFinite(Value.Yaw) && FMath::IsFinite(Value.Roll);
}
