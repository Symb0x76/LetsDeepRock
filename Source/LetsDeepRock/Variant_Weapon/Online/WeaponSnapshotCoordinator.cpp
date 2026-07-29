#include "WeaponSnapshotCoordinator.h"

#include "AbilitySystemComponent.h"
#include "AttributeModifierManager.h"
#include "EnemyCharacter.h"
#include "EngineUtils.h"
#include "Engine/World.h"
#include "GameDataSubsystem.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
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
#include "Weapons/WeaponEquipmentComponent.h"

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

bool UWeaponSnapshotCoordinator::SerializeSaveGame(
	UWeaponMatchSaveGame* SaveGame,
	TArray<uint8>& OutData,
	FString& OutError) const
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

bool UWeaponSnapshotCoordinator::ValidateSaveGame(
	const UWeaponMatchSaveGame* SaveGame,
	FString& OutError) const
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

	if (const FString CurrentMapPackage = UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()); CurrentMapPackage != SaveGame->MapPackageName)
	{
		OutError = FString::Printf(
			TEXT("SaveGame requires map %s but current map is %s"),
			*SaveGame->MapPackageName,
			*CurrentMapPackage);
		return false;
	}

	if (!PreflightSnapshotAssets(SaveGame->Snapshot, OutError))
	{
		return false;
	}

	if (AWeaponGameState* GameState = World->GetGameState<AWeaponGameState>(); !GameState || !GameState->SetMaxPlayersAuthority(SaveGame->MaxPlayers))
	{
		OutError = TEXT("Failed to apply SaveGame player capacity to AWeaponGameState");
		return false;
	}

	return RestoreSnapshot(SaveGame->Snapshot, OutError);
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
		!ValidateSnapshot(MatchSnapshot, GameState->GetMaxPlayers(), OutError))
	{
		return false;
	}

	OutSnapshot = MoveTemp(MatchSnapshot);
	return true;
}

bool UWeaponSnapshotCoordinator::CapturePlayers(
	const UWorld* World,
	FWeaponSnapshot& Snapshot,
	FString& OutError) const
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
			OutError = FString::Printf(TEXT("Player %s does not have WeaponEquipmentComponent"), *Player.PlayerName);
			return false;
		}
		Player.CurrentWeaponIndex = Equipment->GetCurrentWeaponIndex();
	}

	return true;
}

bool UWeaponSnapshotCoordinator::CaptureEnemies(
	const UWorld* World,
	FWeaponSnapshot& Snapshot,
	FString& OutError) const
{
	for (TActorIterator<AEnemyCharacter> It(World); It; ++It)
	{
		const AEnemyCharacter* Enemy = *It;
		FWeaponEnemySnapshot& EnemySnapshot = Snapshot.Enemies.AddDefaulted_GetRef();
		EnemySnapshot.EnemyClass = FSoftClassPath(Enemy->GetClass());
		EnemySnapshot.Location = Enemy->GetActorLocation();
		EnemySnapshot.Rotation = FRotator(0.0f, Enemy->GetActorRotation().Yaw, 0.0f);
		EnemySnapshot.Velocity = Enemy->GetVelocity();
		EnemySnapshot.EnemyConfigRowName = Enemy->GetEnemyConfigRowName();
		EnemySnapshot.AttributeState.Values = CaptureAttributeValues(Enemy->GetAbilitySystemComponent());
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

bool UWeaponSnapshotCoordinator::ValidateAttributeValues(const TMap<FGameplayTag, float>& Values)
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

bool UWeaponSnapshotCoordinator::ValidateSnapshot(
	const FWeaponSnapshot& Snapshot,
	const int32 MaxPlayers,
	FString& OutError) const
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

	TSet<FString> PlayerIds;
	TSet<int32> Slots;
	for (const FWeaponPlayerSnapshot& Player : Snapshot.Players)
	{
		if (Player.PlayerId.IsEmpty() || Player.PlayerName.IsEmpty() ||
			PlayerIds.Contains(Player.PlayerId) || Slots.Contains(Player.SlotIndex) ||
			Player.SlotIndex < 0 || Player.SlotIndex >= MaxPlayers ||
			!ValidateAttributeValues(Player.AttributeState.Values))
		{
			OutError = TEXT("Snapshot has invalid or duplicated player identity, slot or attributes");
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

	for (const FWeaponEnemySnapshot& Enemy : Snapshot.Enemies)
	{
		if (!Enemy.EnemyClass.IsValid() || Enemy.EnemyConfigRowName.IsNone() ||
			!IsFiniteVector(Enemy.Location) || !IsFiniteRotator(Enemy.Rotation) ||
			!IsFiniteVector(Enemy.Velocity) || !ValidateAttributeValues(Enemy.AttributeState.Values))
		{
			OutError = TEXT("Snapshot has invalid enemy class, config, movement or attributes");
			return false;
		}
	}

	return true;
}

bool UWeaponSnapshotCoordinator::PreflightSnapshotAssets(
	const FWeaponSnapshot& Snapshot,
	FString& OutError) const
{
	const UGameInstance* GameInstance = GetGameInstance();
	const UGameDataSubsystem* DataSubsystem = GameInstance
		? GameInstance->GetSubsystem<UGameDataSubsystem>()
		: nullptr;
	if (!DataSubsystem)
	{
		OutError = TEXT("GameDataSubsystem is unavailable during Snapshot preflight");
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
			OutError = FString::Printf(TEXT("Character config row %s does not exist"), *Player.CharacterConfigRowName.ToString());
			return false;
		}
	}

	for (const FWeaponEnemySnapshot& Enemy : Snapshot.Enemies)
	{
		if (!Enemy.EnemyClass.TryLoadClass<AEnemyCharacter>() ||
			!DataSubsystem->GetEnemyConfig(Enemy.EnemyConfigRowName))
		{
			OutError = FString::Printf(TEXT("Enemy class or config %s could not be loaded"), *Enemy.EnemyConfigRowName.ToString());
			return false;
		}
	}
	return true;
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
		!RestorePlayers(Snapshot, OutError) ||
		!RestoreEnemies(Snapshot, OutError))
	{
		return false;
	}

	GameState->SetMatchPhase(Snapshot.MatchPhase, Snapshot.RemainingPhaseTime);
	ActiveMatchId = Snapshot.MatchId;
	NextSnapshotRevision = Snapshot.Revision + 1;
	return true;
}

bool UWeaponSnapshotCoordinator::RestorePlayers(
	const FWeaponSnapshot& Snapshot,
	FString& OutError) const
{
	const UWorld* World = GetWorld();
	AWeaponGameState* GameState = World->GetGameState<AWeaponGameState>();
	AWeaponGameMode* GameMode = World->GetAuthGameMode<AWeaponGameMode>();

	TMap<FString, AWeaponPlayerState*> PlayersById;
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
			OutError = FString::Printf(TEXT("Connected player %s is not ready for Snapshot Apply"), *Snap.PlayerId);
			return false;
		}
		if (Snap.bHasPawn && !(*FoundPlayerState)->GetPlayerController())
		{
			OutError = FString::Printf(TEXT("Player %s does not have a PlayerController"), *Snap.PlayerId);
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
				OutError = FString::Printf(TEXT("Failed to restore attributes for player %s"), *PlayerId);
				return false;
			}
			continue;
		}

		if (!PlayerController)
		{
			OutError = FString::Printf(TEXT("Player %s does not have a PlayerController"), *PlayerId);
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
			OutError = FString::Printf(TEXT("Failed to prepare Character for player %s"), *PlayerId);
			return false;
		}

		Character->SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
		Character->SetAimDirectionAuthoritative(Rotation.Vector());
		Character->GetCharacterMovement()->Velocity = Velocity;

		UWeaponEquipmentComponent* Equipment = Character->FindComponentByClass<UWeaponEquipmentComponent>();
		if (!Equipment || !Equipment->RestoreWeaponSelection(CurrentWeaponIndex) ||
			!RestoreAttributeValues(PlayerState->GetAbilitySystemComponent(), AttributeState.Values))
		{
			OutError = FString::Printf(TEXT("Failed to restore equipment or attributes for player %s"), *PlayerId);
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

FString UWeaponSnapshotCoordinator::ResolvePlayerId(const APlayerState* PlayerState)
{
	return PlayerState ? PlayerState->GetPlayerName() : FString();
}

bool UWeaponSnapshotCoordinator::IsFiniteVector(const FVector& Value)
{
	return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
}

bool UWeaponSnapshotCoordinator::IsFiniteRotator(const FRotator& Value)
{
	return FMath::IsFinite(Value.Pitch) && FMath::IsFinite(Value.Yaw) && FMath::IsFinite(Value.Roll);
}
