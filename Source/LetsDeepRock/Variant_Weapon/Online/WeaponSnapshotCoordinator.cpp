#include "WeaponSnapshotCoordinator.h"
#include "AbilitySystemComponent.h"
#include "AttributeModifierManager.h"
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
#include "WeaponCharacter.h"
#include "WeaponGameMode.h"
#include "WeaponGameState.h"
#include "WeaponMatchSaveGame.h"
#include "WeaponPlayerState.h"
#include "WeaponGameplayTags.h"
#include "WeaponSnapshot.h"
#include "Engine/GameInstance.h"
#include "UObject/Package.h"
#include "Weapons/GrenadeProjectile.h"
#include "Weapons/WeaponEquipmentComponent.h"
#include "Weapons/WeaponGrenadeDataAsset.h"
#include "Weapons/WeaponProjectile.h"

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
		UAbilitySystemComponent* ASC = nullptr;
		bool bCharacterStaged = false;
	};

	UWorld* World = nullptr;
	AWeaponGameState* GameState = nullptr;
	AWeaponGameMode* GameMode = nullptr;
	AGridMapManager* GridMap = nullptr;
	UClass* GridGeneratorClass = nullptr;

	TArray<FPlayerTarget> Players;
	TMap<FUniqueNetIdRepl, APawn*> FinalPawnsByPlayerId;
	TMap<FUniqueNetIdRepl, int32> SlotReservations;

	TArray<UClass*> EnemyClasses;
	TArray<AEnemyCharacter*> ExistingEnemies;
	TArray<AEnemyCharacter*> StagedEnemies;

	TArray<UClass*> ProjectileClasses;
	TArray<TSubclassOf<UGameplayEffect>> ProjectileDamageEffects;
	TArray<const UWeaponGrenadeDataAsset*> GrenadeDataAssets;
	TArray<AWeaponProjectile*> ExistingProjectiles;
	TArray<AWeaponProjectile*> StagedProjectiles;
};

bool UWeaponSnapshotCoordinator::CaptureSaveGame(
	const UWorld* World,
	UWeaponMatchSaveGame*& OutSaveGame,
	FString& OutError)
{
	OutSaveGame = nullptr;
	OutError.Reset();

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
		OutContext.GameState->PlayerArray.Num() > SaveGame->MaxPlayers)
	{
		OutError = TEXT("Restore target is missing authoritative match owners or exceeds saved capacity");
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

	TMap<FUniqueNetIdRepl, AWeaponPlayerState*> PlayersById;
	for (APlayerState* BasePlayerState : OutContext.GameState->PlayerArray)
	{
		AWeaponPlayerState* PlayerState = Cast<AWeaponPlayerState>(BasePlayerState);
		const FUniqueNetIdRepl PlayerId = ResolvePlayerId(PlayerState);
		if (!PlayerState || !PlayerId.IsValid() || PlayersById.Contains(PlayerId))
		{
			OutError = TEXT("Restore target contains an invalid, duplicated, or incompatible PlayerState");
			return false;
		}
		PlayersById.Add(PlayerId, PlayerState);
	}
	if (PlayersById.Num() != Snapshot.Players.Num())
	{
		OutError = TEXT("All Snapshot players must be connected before this synchronous Restore can run");
		return false;
	}

	OutContext.Players.Reserve(Snapshot.Players.Num());
	for (const FWeaponPlayerSnapshot& Player : Snapshot.Players)
	{
		AWeaponPlayerState* const* FoundPlayerState = PlayersById.Find(Player.PlayerId);
		if (!FoundPlayerState)
		{
			OutError = FString::Printf(TEXT("Connected player %s is not ready for Snapshot Apply"),
				*Player.PlayerId.ToDebugString());
			return false;
		}

		FRestoreContext::FPlayerTarget& Target = OutContext.Players.AddDefaulted_GetRef();
		Target.Snapshot = &Player;
		Target.PlayerState = *FoundPlayerState;
		Target.PlayerController = Target.PlayerState->GetPlayerController();
		Target.ExistingPawn = Target.PlayerController
			? Target.PlayerController->GetPawn().Get()
			: Target.PlayerState->GetPawn();
		Target.ASC = Target.PlayerState->GetAbilitySystemComponent();
		OutContext.SlotReservations.Add(Player.PlayerId, Player.SlotIndex);

		if (!CanRestoreAttributeValues(Target.ASC, Player.AttributeState.Values))
		{
			OutError = FString::Printf(TEXT("Player %s ASC is incompatible with Snapshot attributes"),
				*Player.PlayerId.ToDebugString());
			return false;
		}

		if (!Player.bHasPawn)
		{
			continue;
		}
		if (!Target.PlayerController)
		{
			OutError = FString::Printf(TEXT("Player %s does not have a PlayerController"),
				*Player.PlayerId.ToDebugString());
			return false;
		}

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
			if (!Target.Character->GetCharacterMovement() || !Target.Equipment ||
				!Target.Equipment->CanRestoreWeaponSelection(Player.CurrentWeaponIndex))
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
			!DamageEffectClass ||
			(!OutContext.FinalPawnsByPlayerId.Contains(Projectile.InstigatorPlayerId) &&
				!PlayersById.Contains(Projectile.InstigatorPlayerId)))
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
	if (!ActiveMatchId.IsValid())
	{
		ActiveMatchId = FGuid::NewGuid();
	}
	MatchSnapshot.MatchId = ActiveMatchId;
	MatchSnapshot.Revision = NextSnapshotRevision++;
	MatchSnapshot.MatchPhase = GameState->GetMatchPhase();
	MatchSnapshot.RemainingPhaseTime = GameState->GetRemainingPhaseTime();

	if (!CapturePlayers(World, MatchSnapshot, OutError) ||
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
		!Snapshot.MatchId.IsValid() || !FMath::IsFinite(Snapshot.RemainingPhaseTime))
	{
		OutError = TEXT("Snapshot version, identity, revision or phase time is invalid");
		return false;
	}
	if (MaxPlayers < 1 || MaxPlayers > 4 || Snapshot.Players.IsEmpty() || Snapshot.Players.Num() > MaxPlayers)
	{
		OutError = TEXT("Snapshot player count or SaveGame capacity is invalid");
		return false;
	}

	return ValidatePlayers(Snapshot, MaxPlayers, OutError) &&
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
		if (!Character->SetCharacterConfigRowNameAuthoritative(Player.CharacterConfigRowName))
		{
			OutError = FString::Printf(TEXT("Failed to stage Character config for player %s"),
				*Player.PlayerId.ToDebugString());
			return false;
		}

		Target.Equipment = Character->FindComponentByClass<UWeaponEquipmentComponent>();
		if (!Character->GetCharacterMovement() || !Target.Equipment ||
			!Target.Equipment->CanRestoreWeaponSelection(Player.CurrentWeaponIndex))
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
			SavedProjectile.ProjectileSpeed);
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
		Grenade->InitFromGrenadeData(Context.GrenadeDataAssets[Index], *FoundInstigator);
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
			Target.Character->Destroy();
			Target.Character = nullptr;
			Target.Equipment = nullptr;
			Target.bCharacterStaged = false;
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
		else if (!Target.Character->SetCharacterConfigRowNameAuthoritative(Player.CharacterConfigRowName))
		{
			UE_LOG(LogTemp, Fatal, TEXT("Validated Character config became invalid during Snapshot Apply"));
		}

		Target.Character->SetActorLocation(Player.Location, false, nullptr, ETeleportType::TeleportPhysics);
		Target.Character->SetAimDirectionAuthoritative(Player.Rotation.Vector());
		Target.Character->GetCharacterMovement()->Velocity = Player.Velocity;
		Target.Equipment->RestoreWeaponSelection(Player.CurrentWeaponIndex);
		RestoreAttributeValues(Target.ASC, Player.AttributeState.Values);
		if (Target.bCharacterStaged)
		{
			Target.Character->SetActorTickEnabled(true);
			Target.Character->SetActorEnableCollision(true);
			Target.Character->ForceNetUpdate();
		}
	}

	for (int32 Index = 0; Index < Context.StagedEnemies.Num(); ++Index)
	{
		AEnemyCharacter* Enemy = Context.StagedEnemies[Index];
		const FWeaponEnemySnapshot& EnemySnap = Snapshot.Enemies[Index];
		Enemy->GetCharacterMovement()->Velocity = EnemySnap.Velocity;
		RestoreAttributeValues(Enemy->GetAbilitySystemComponent(), EnemySnap.AttributeState.Values);
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
		Projectile->SetActorTickEnabled(true);
		Projectile->SetActorEnableCollision(true);
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
		Target.PlayerState->SetPlayerName(Player.PlayerName);
		Target.PlayerState->SetPlayerSlotIndex(Player.SlotIndex);
		Target.PlayerState->SetPlayerReady(Player.bIsReady);
		Target.PlayerState->SetTeamId(Player.TeamId);
	}

	if (SaveGame->MaxPlayers < PreviousMaxPlayers &&
		!Context.GameState->SetMaxPlayersAuthority(SaveGame->MaxPlayers))
	{
		UE_LOG(LogTemp, Fatal, TEXT("Validated player capacity became invalid during Snapshot Apply"));
	}
	if (!Context.GameMode->SetPlayerSlotReservations(Context.SlotReservations))
	{
		UE_LOG(LogTemp, Fatal, TEXT("Validated Slot reservations became invalid during Snapshot Apply"));
	}

	Context.GameState->SetMatchPhase(Snapshot.MatchPhase, Snapshot.RemainingPhaseTime);
	ActiveMatchId = Snapshot.MatchId;
	NextSnapshotRevision = Snapshot.Revision + 1;
}

bool UWeaponSnapshotCoordinator::CapturePlayers(
	const UWorld* World,
	FWeaponSnapshot& Snapshot,
	FString& OutError)
{
	const AWeaponGameState* GameState = World->GetGameState<AWeaponGameState>();
	for (APlayerState* BasePlayerState : GameState->PlayerArray)
	{
		const AWeaponPlayerState* PlayerState = Cast<AWeaponPlayerState>(BasePlayerState);
		if (!PlayerState)
		{
			continue;
		}

		FWeaponPlayerSnapshot& Player = Snapshot.Players.AddDefaulted_GetRef();
		Player.PlayerId = ResolvePlayerId(PlayerState);
		Player.PlayerName = PlayerState->GetPlayerName();
		Player.SlotIndex = PlayerState->GetPlayerSlotIndex();
		Player.bIsReady = PlayerState->GetIsPlayerReady();
		Player.TeamId = PlayerState->GetTeamId();
		Player.AttributeState.Values = CaptureAttributeValues(PlayerState->GetAbilitySystemComponent());

		const AWeaponCharacter* Character = Cast<AWeaponCharacter>(PlayerState->GetPawn());
		if (!Character)
		{
			continue;
		}

		Player.bHasPawn = true;
		Player.CharacterConfigRowName = Character->GetCharacterConfigRowName();
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
		Player.CurrentWeaponIndex = Equipment->GetCurrentWeaponIndex();
	}

	return true;
}

bool UWeaponSnapshotCoordinator::ValidatePlayers(const FWeaponSnapshot& Snapshot,
	const int32 MaxPlayers,
	FString& OutError)
{
	TSet<FUniqueNetIdRepl> PlayerIds;
	TSet<int32> Slots;
	for (const FWeaponPlayerSnapshot& Player : Snapshot.Players)
	{
		if (!Player.PlayerId.IsValid() || Player.PlayerName.IsEmpty() ||
			PlayerIds.Contains(Player.PlayerId) || Slots.Contains(Player.SlotIndex) ||
			Player.SlotIndex < 0 || Player.SlotIndex >= MaxPlayers ||
			!ValidateAttributeValues(Player.AttributeState.Values))
		{
			OutError = TEXT("Snapshot player has invalid or duplicated identity, slot or attributes");
			return false;
		}

		if (Player.bHasPawn)
		{
			if (Player.CharacterConfigRowName.IsNone() || !IsFiniteVector(Player.Location) ||
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
		}
		else if (Player.CurrentWeaponIndex != INDEX_NONE)
		{
			OutError = TEXT("Snapshot player without Pawn cannot select a Pawn weapon");
			return false;
		}

		PlayerIds.Add(Player.PlayerId);
		Slots.Add(Player.SlotIndex);
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
		auto& [EnemyClass, Location, Rotation, Velocity, EnemyConfigRowName, AttributeState] = Snapshot.Enemies.AddDefaulted_GetRef();
		EnemyClass = FSoftClassPath(Enemy->GetClass());
		Location = Enemy->GetActorLocation();
		Rotation = FRotator(0.0f, Enemy->GetActorRotation().Yaw, 0.0f);
		Velocity = Enemy->GetVelocity();
		EnemyConfigRowName = Enemy->GetEnemyConfigRowName();
		AttributeState.Values = CaptureAttributeValues(Enemy->GetAbilitySystemComponent());
	}
	return true;
}

bool UWeaponSnapshotCoordinator::ValidateEnemies(const FWeaponSnapshot& Snapshot, FString& OutError)
{
	for (const auto& [EnemyClass, Location, Rotation, Velocity, EnemyConfigRowName, AttributeState] : Snapshot.Enemies)
	{
		if (!EnemyClass.IsValid() || EnemyConfigRowName.IsNone() ||
			!IsFiniteVector(Location) || !IsFiniteRotator(Rotation) ||
			!IsFiniteVector(Velocity) || !ValidateAttributeValues(AttributeState.Values))
		{
			OutError = TEXT("Snapshot has invalid enemy class, config, movement or attributes");
			return false;
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
		const FUniqueNetIdRepl InstigatorPlayerId = ResolvePlayerIdFromActor(Initialization.InstigatorActor);
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
		SavedProjectile.DamageValue = Initialization.DamageValue;
		SavedProjectile.ProjectileSpeed = Initialization.ProjectileSpeed;
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
			Projectile.ProjectileSpeed <= 0.0f || !FMath::IsFinite(Projectile.RemainingLifeSeconds) ||
			Projectile.RemainingLifeSeconds <= 0.0f)
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
			Result.Add(Tag, ASC->GetNumericAttribute(Attribute));
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

FUniqueNetIdRepl UWeaponSnapshotCoordinator::ResolvePlayerId(const APlayerState* PlayerState)
{
	return PlayerState ? PlayerState->GetUniqueId() : FUniqueNetIdRepl();
}

FUniqueNetIdRepl UWeaponSnapshotCoordinator::ResolvePlayerIdFromActor(const AActor* Actor)
{
	if (const APlayerState* PlayerState = Cast<APlayerState>(Actor))
	{
		return ResolvePlayerId(PlayerState);
	}
	if (const APawn* Pawn = Cast<APawn>(Actor))
	{
		return ResolvePlayerId(Pawn->GetPlayerState());
	}
	if (const AController* Controller = Cast<AController>(Actor))
	{
		return ResolvePlayerId(Controller->GetPlayerState<APlayerState>());
	}
	return FUniqueNetIdRepl();
}

bool UWeaponSnapshotCoordinator::IsFiniteVector(const FVector& Value)
{
	return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
}

bool UWeaponSnapshotCoordinator::IsFiniteRotator(const FRotator& Value)
{
	return FMath::IsFinite(Value.Pitch) && FMath::IsFinite(Value.Yaw) && FMath::IsFinite(Value.Roll);
}
