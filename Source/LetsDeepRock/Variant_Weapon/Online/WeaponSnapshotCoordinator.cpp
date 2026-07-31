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
	if (!ValidateRestore(SaveGame, OutError))
	{
		return false;
	}

	const UWorld* World = GetWorld();
	if (AWeaponGameState* GameState = World->GetGameState<AWeaponGameState>(); !GameState || !GameState->SetMaxPlayersAuthority(SaveGame->MaxPlayers))
	{
		OutError = TEXT("Failed to apply SaveGame player capacity to AWeaponGameState");
		return false;
	}

	return RestoreSnapshot(SaveGame->Snapshot, OutError);
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
	FString& OutError) const
{
	if (!ValidateSaveGame(SaveGame, OutError))
	{
		return false;
	}

	const UWorld* World = GetWorld();
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

	const FWeaponSnapshot& Snapshot = SaveGame->Snapshot;
	int32 GridMapCount = 0;
	for (TActorIterator<AGridMapManager> It(World); It; ++It)
	{
		++GridMapCount;
	}
	if (GridMapCount != 1)
	{
		OutError = TEXT("Snapshot Restore requires exactly one GridMapManager");
		return false;
	}

	if (!Snapshot.GridMap.GeneratorClass.TryLoadClass<UGridMapGenerator>())
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

	for (const FWeaponPlayerSnapshot& Player : Snapshot.Players)
	{
		if (!Player.bHasPawn)
		{
			continue;
		}

		if (!DataSubsystem->GetCharacterAttributes(Player.CharacterConfigRowName))
		{
			OutError = FString::Printf(TEXT("Character config row %s does not exist"),
				*Player.CharacterConfigRowName.ToString());
			return false;
		}
	}

	for (const FWeaponEnemySnapshot& Enemy : Snapshot.Enemies)
	{
		if (!Enemy.EnemyClass.TryLoadClass<AEnemyCharacter>() ||
			!DataSubsystem->GetEnemyConfig(Enemy.EnemyConfigRowName))
		{
			OutError = FString::Printf(TEXT("Enemy class or config %s could not be loaded"),
				*Enemy.EnemyConfigRowName.ToString());
			return false;
		}
	}

	for (const FWeaponProjectileSnapshot& Projectile : Snapshot.Projectiles)
	{
		UClass* ProjectileClass = Projectile.ProjectileClass.TryLoadClass<AWeaponProjectile>();
		if (!ProjectileClass || ProjectileClass->HasAnyClassFlags(CLASS_Abstract) ||
			ProjectileClass->IsChildOf(AGrenadeProjectile::StaticClass()) ||
			!Projectile.DamageEffectClass.TryLoadClass<UGameplayEffect>())
		{
			OutError = FString::Printf(TEXT("Projectile class or damage effect %s could not be loaded"),
				*Projectile.ProjectileClass.ToString());
			return false;
		}
	}

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

bool UWeaponSnapshotCoordinator::RestoreSnapshot(
	const FWeaponSnapshot& Snapshot,
	FString& OutError)
{
	OutError.Reset();
	const UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client)
	{
		OutError = TEXT("Snapshot could only be restored into a valid server World");
		return false;
	}

	AWeaponGameState* GameState = World->GetGameState<AWeaponGameState>();
	if (!GameState)
	{
		OutError = TEXT("Current World does not have AWeaponGameState");
		return false;
	}
	if (GameState->PlayerArray.Num() != Snapshot.Players.Num())
	{
		OutError = TEXT("All Snapshot players must be connected before this synchronous Restore can run");
		return false;
	}
	if (!ValidateSnapshot(Snapshot, GameState->GetMaxPlayers(), OutError) ||
		!RestoreGridMap(Snapshot, OutError) ||
		!RestorePlayers(Snapshot, OutError) ||
		!RestoreEnemies(Snapshot, OutError) ||
		!RestoreProjectiles(Snapshot, OutError))
	{
		return false;
	}

	TMap<FUniqueNetIdRepl, int32> RestoredSlotReservations;
	for (const FWeaponPlayerSnapshot& Player : Snapshot.Players)
	{
		RestoredSlotReservations.Add(Player.PlayerId, Player.SlotIndex);
	}
	AWeaponGameMode* GameMode = World->GetAuthGameMode<AWeaponGameMode>();
	if (!GameMode || !GameMode->SetPlayerSlotReservations(RestoredSlotReservations))
	{
		OutError = TEXT("Failed to publish restored stable player Slot reservations");
		return false;
	}

	GameState->SetMatchPhase(Snapshot.MatchPhase, Snapshot.RemainingPhaseTime);
	ActiveMatchId = Snapshot.MatchId;
	NextSnapshotRevision = Snapshot.Revision + 1;
	return true;
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

bool UWeaponSnapshotCoordinator::RestoreGridMap(
	const FWeaponSnapshot& Snapshot,
	FString& OutError) const
{
	AGridMapManager* GridMap = nullptr;
	for (TActorIterator<AGridMapManager> It(GetWorld()); It; ++It)
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
	return GridMap->RestoreRecoveryState(Snapshot.GridMap, OutError);
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

	for (const auto& [Tag, Value] : Values)
	{
		const FGameplayAttribute* Attribute = AttributeMap.Find(Tag);
		if (!Tag.IsValid() || !Attribute || !Attribute->IsValid() || !FMath::IsFinite(Value))
		{
			return false;
		}
	}
	return true;
}

bool UWeaponSnapshotCoordinator::RestoreAttributeValues(
	UAbilitySystemComponent* ASC,
	const TMap<FGameplayTag, float>& Values)
{
	if (!ASC)
	{
		return false;
	}
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
		if (Value && (!Attribute || !ASC->HasAttributeSetForAttribute(*Attribute) || !ValidateAttributeValues(Values)))
		{
			return false;
		}
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
		if (!Attribute || !ASC->HasAttributeSetForAttribute(*Attribute) || !FMath::IsFinite(Value))
		{
			return false;
		}
		ASC->SetNumericAttributeBase(*Attribute, Value);
	}
	return true;
}

bool UWeaponSnapshotCoordinator::RestorePlayers(
	const FWeaponSnapshot& Snapshot,
	FString& OutError) const
{
	const UWorld* World = GetWorld();
	AWeaponGameState* GameState = World->GetGameState<AWeaponGameState>();
	AWeaponGameMode* GameMode = World->GetAuthGameMode<AWeaponGameMode>();

	TMap<FUniqueNetIdRepl, AWeaponPlayerState*> PlayersById;
	for (APlayerState* BasePlayerState : GameState->PlayerArray)
	{
		if (AWeaponPlayerState* PlayerState = Cast<AWeaponPlayerState>(BasePlayerState))
		{
			PlayersById.Add(ResolvePlayerId(PlayerState), PlayerState);
		}
	}
	for (const FWeaponPlayerSnapshot& Snap : Snapshot.Players)
	{
		AWeaponPlayerState* const* FoundPlayerState = PlayersById.Find(Snap.PlayerId);
		if (!FoundPlayerState)
		{
			OutError = FString::Printf(TEXT("Connected player %s is not ready for Snapshot Apply"),
				*Snap.PlayerId.ToDebugString());
			return false;
		}
		if (Snap.bHasPawn && !(*FoundPlayerState)->GetPlayerController())
		{
			OutError = FString::Printf(TEXT("Player %s does not have a PlayerController"),
				*Snap.PlayerId.ToDebugString());
			return false;
		}
	}

	for (const auto& [PlayerId, PlayerName, SlotIndex, bIsReady, bHasPawn, TeamId, Location, Rotation, Velocity, CharacterConfigRowName, CurrentWeaponIndex, AttributeState] : Snapshot.Players)
	{
		AWeaponPlayerState* PlayerState = PlayersById.FindChecked(PlayerId);
		PlayerState->SetPlayerName(PlayerName);
		PlayerState->SetPlayerSlotIndex(SlotIndex);
		PlayerState->SetPlayerReady(bIsReady);
		PlayerState->SetTeamId(TeamId);

		APlayerController* PlayerController = PlayerState->GetPlayerController();
		if (!bHasPawn)
		{
			if (PlayerController && PlayerController->GetPawn())
			{
				APawn* ExistingPawn = PlayerController->GetPawn();
				if (UWeaponEquipmentComponent* Equipment = ExistingPawn->FindComponentByClass<UWeaponEquipmentComponent>())
				{
					Equipment->UnequipWeapon();
				}
				PlayerController->UnPossess();
				ExistingPawn->Destroy();
			}

			UAbilitySystemComponent* ASC = PlayerState->GetAbilitySystemComponent();
			if (ASC)
			{
				// PlayerState remains the GAS owner, but a no-Pawn Snapshot must not retain a destroyed Avatar.
				ASC->InitAbilityActorInfo(PlayerState, nullptr);
			}
			if (!RestoreAttributeValues(ASC, AttributeState.Values))
			{
				OutError = FString::Printf(TEXT("Failed to restore attributes for player %s"),
					*PlayerId.ToDebugString());
				return false;
			}
			continue;
		}

		if (!PlayerController)
		{
			OutError = FString::Printf(TEXT("Player %s does not have a PlayerController"),
				*PlayerId.ToDebugString());
			return false;
		}

		AWeaponCharacter* Character = Cast<AWeaponCharacter>(PlayerController->GetPawn());
		if (!Character && GameMode)
		{
			if (APawn* NewPawn = GameMode->SpawnDefaultPawnFor(PlayerController, nullptr))
			{
				PlayerController->Possess(NewPawn);
				Character = Cast<AWeaponCharacter>(NewPawn);
			}
		}
		if (!Character || !Character->SetCharacterConfigRowNameAuthoritative(CharacterConfigRowName))
		{
			OutError = FString::Printf(TEXT("Failed to prepare Character for player %s"),
				*PlayerId.ToDebugString());
			return false;
		}

		Character->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
		Character->SetAimDirectionAuthoritative(Rotation.Vector());
		Character->GetCharacterMovement()->Velocity = Velocity;

		UWeaponEquipmentComponent* Equipment = Character->FindComponentByClass<UWeaponEquipmentComponent>();
		if (!Equipment || !Equipment->RestoreWeaponSelection(CurrentWeaponIndex) ||
			!RestoreAttributeValues(PlayerState->GetAbilitySystemComponent(), AttributeState.Values))
		{
			OutError = FString::Printf(TEXT("Failed to restore equipment or attributes for player %s"),
				*PlayerId.ToDebugString());
			return false;
		}
	}

	return true;
}

bool UWeaponSnapshotCoordinator::RestoreEnemies(
	const FWeaponSnapshot& Snapshot,
	FString& OutError) const
{
	UWorld* World = GetWorld();
	TArray<UClass*> EnemyClasses;
	EnemyClasses.Reserve(Snapshot.Enemies.Num());
	for (const FWeaponEnemySnapshot& EnemySnap : Snapshot.Enemies)
	{
		UClass* EnemyClass = EnemySnap.EnemyClass.TryLoadClass<AEnemyCharacter>();
		if (!EnemyClass)
		{
			OutError = FString::Printf(TEXT("Failed to load enemy class %s"), *EnemySnap.EnemyClass.ToString());
			return false;
		}
		EnemyClasses.Add(EnemyClass);
	}

	TArray<AEnemyCharacter*> ExistingEnemies;
	for (TActorIterator<AEnemyCharacter> It(World); It; ++It)
	{
		ExistingEnemies.Add(*It);
	}
	for (AEnemyCharacter* Enemy : ExistingEnemies)
	{
		Enemy->Destroy();
	}

	for (int32 Index = 0; Index < Snapshot.Enemies.Num(); ++Index)
	{
		const FWeaponEnemySnapshot& EnemySnap = Snapshot.Enemies[Index];
		const FTransform SpawnTransform(EnemySnap.Rotation, EnemySnap.Location);
		AEnemyCharacter* Enemy = World->SpawnActorDeferred<AEnemyCharacter>(
			EnemyClasses[Index],
			SpawnTransform,
			nullptr,
			nullptr,
			ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn);
		if (!Enemy)
		{
			OutError = FString::Printf(TEXT("Failed to spawn enemy %d"), Index);
			return false;
		}

		Enemy->SetEnemyConfigRowName(EnemySnap.EnemyConfigRowName);
		Enemy->FinishSpawning(SpawnTransform);
		Enemy->GetCharacterMovement()->Velocity = EnemySnap.Velocity;
		if (!RestoreAttributeValues(Enemy->GetAbilitySystemComponent(), EnemySnap.AttributeState.Values))
		{
			OutError = FString::Printf(TEXT("Failed to restore attributes for enemy %d"), Index);
			return false;
		}
	}

	return true;
}

bool UWeaponSnapshotCoordinator::RestoreProjectiles(
	const FWeaponSnapshot& Snapshot,
	FString& OutError) const
{
	UWorld* World = GetWorld();
	const AWeaponGameState* GameState = World ? World->GetGameState<AWeaponGameState>() : nullptr;
	if (!World || !GameState)
	{
		OutError = TEXT("Projectile Restore requires a valid World and AWeaponGameState");
		return false;
	}

	TMap<FUniqueNetIdRepl, APawn*> PawnsByPlayerId;
	for (APlayerState* BasePlayerState : GameState->PlayerArray)
	{
		if (AWeaponPlayerState* PlayerState = Cast<AWeaponPlayerState>(BasePlayerState))
		{
			PawnsByPlayerId.Add(ResolvePlayerId(PlayerState), PlayerState->GetPawn());
		}
	}

	TArray<AWeaponProjectile*> ExistingProjectiles;
	for (TActorIterator<AWeaponProjectile> It(World); It; ++It)
	{
		if (!It->IsActorBeingDestroyed())
		{
			ExistingProjectiles.Add(*It);
		}
	}

	TArray<AWeaponProjectile*> StagedProjectiles;
	StagedProjectiles.Reserve(Snapshot.Projectiles.Num() + Snapshot.Grenades.Num());
	const auto AbortStaging = [&StagedProjectiles]()
	{
		for (AWeaponProjectile* Projectile : StagedProjectiles)
		{
			if (Projectile)
			{
				Projectile->Destroy();
			}
		}
	};

	for (int32 Index = 0; Index < Snapshot.Projectiles.Num(); ++Index)
	{
		const FWeaponProjectileSnapshot& SavedProjectile = Snapshot.Projectiles[Index];
		APawn* const* FoundInstigator = PawnsByPlayerId.Find(SavedProjectile.InstigatorPlayerId);
		UClass* ProjectileClass = SavedProjectile.ProjectileClass.TryLoadClass<AWeaponProjectile>();
		TSubclassOf<UGameplayEffect> DamageEffectClass =
			SavedProjectile.DamageEffectClass.TryLoadClass<UGameplayEffect>();
		if (!FoundInstigator || !*FoundInstigator || !ProjectileClass || !DamageEffectClass)
		{
			AbortStaging();
			OutError = FString::Printf(TEXT("Failed to resolve projectile %d dependencies"), Index);
			return false;
		}

		const FTransform SpawnTransform(SavedProjectile.Rotation, SavedProjectile.Location);
		AWeaponProjectile* Projectile = World->SpawnActorDeferred<AWeaponProjectile>(
			ProjectileClass,
			FTransform::Identity,
			*FoundInstigator,
			*FoundInstigator,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Projectile)
		{
			AbortStaging();
			OutError = FString::Printf(TEXT("Failed to stage projectile %d"), Index);
			return false;
		}

		StagedProjectiles.Add(Projectile);
		Projectile->SetActorEnableCollision(false);
		Projectile->InitializeProjectile(
			DamageEffectClass,
			SavedProjectile.DamageValue,
			*FoundInstigator,
			SavedProjectile.DamageTags,
			SavedProjectile.bPenetratesTerrain,
			SavedProjectile.ProjectileSpeed);
		Projectile->FinishSpawning(SpawnTransform);
		Projectile->SetActorEnableCollision(false);
		Projectile->RestoreSnapshotMotion(
			SavedProjectile.Velocity,
			SavedProjectile.RemainingLifeSeconds);
	}

	for (int32 Index = 0; Index < Snapshot.Grenades.Num(); ++Index)
	{
		const FWeaponGrenadeSnapshot& SavedGrenade = Snapshot.Grenades[Index];
		APawn* const* FoundInstigator = PawnsByPlayerId.Find(SavedGrenade.InstigatorPlayerId);
		const UWeaponGrenadeDataAsset* GrenadeData = Cast<UWeaponGrenadeDataAsset>(
			SavedGrenade.GrenadeDataAsset.TryLoad());
		if (!FoundInstigator || !*FoundInstigator || !GrenadeData || !GrenadeData->ProjectileClass)
		{
			AbortStaging();
			OutError = FString::Printf(TEXT("Failed to resolve grenade %d dependencies"), Index);
			return false;
		}

		const FTransform SpawnTransform(SavedGrenade.Rotation, SavedGrenade.Location);
		AGrenadeProjectile* Grenade = World->SpawnActorDeferred<AGrenadeProjectile>(
			GrenadeData->ProjectileClass,
			FTransform::Identity,
			*FoundInstigator,
			*FoundInstigator,
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
		if (!Grenade)
		{
			AbortStaging();
			OutError = FString::Printf(TEXT("Failed to stage grenade %d"), Index);
			return false;
		}

		StagedProjectiles.Add(Grenade);
		Grenade->SetActorEnableCollision(false);
		Grenade->InitFromGrenadeData(GrenadeData, *FoundInstigator);
		Grenade->SetFuseTimeForSnapshotRestore(SavedGrenade.RemainingFuseTime);
		Grenade->FinishSpawning(SpawnTransform);
		Grenade->SetActorEnableCollision(false);
		Grenade->RestoreSnapshotMotion(SavedGrenade.Velocity, 0.0f);
	}

	for (AWeaponProjectile* ExistingProjectile : ExistingProjectiles)
	{
		ExistingProjectile->Destroy();
	}
	for (AWeaponProjectile* StagedProjectile : StagedProjectiles)
	{
		StagedProjectile->SetActorEnableCollision(true);
		StagedProjectile->ForceNetUpdate();
	}
	return true;
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
