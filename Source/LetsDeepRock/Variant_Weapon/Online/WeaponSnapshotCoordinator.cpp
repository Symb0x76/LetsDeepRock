#include "WeaponSnapshotCoordinator.h"

#include "AbilitySystemComponent.h"
#include "AttributeModifierManager.h"
#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerController.h"
#include "WeaponGameState.h"
#include "WeaponGameMode.h"
#include "WeaponPlayerState.h"
#include "WeaponCharacter.h"
#include "EnemyCharacter.h"
#include "WeaponSnapshot.h"
#include "EngineUtils.h"
#include "UObject/Package.h"

bool UWeaponSnapshotCoordinator::CaptureSnapshot(const UWorld* World, FWeaponSnapshot& OutSnapshot, FString& OutError)
{
	OutError.Reset();
	if (!World || World->GetNetMode() == NM_Client)
	{
		OutError = TEXT("Snapshot could only be captured from valid World on server");
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
	MatchSnapshot.MapPackageName = World->GetOutermost()->GetName();
	MatchSnapshot.MatchPhase = GameState->GetMatchPhase();
	MatchSnapshot.RemainingPhaseTime = GameState->GetRemainingPhaseTime();
	MatchSnapshot.MaxPlayers = GameState->GetMaxPlayers();

	if (!FMath::IsFinite(MatchSnapshot.RemainingPhaseTime))
	{
		OutError = TEXT("GameState remaining phase time is not valid");
		return false;
	}

	if (!CapturePlayers(World, MatchSnapshot, OutError) || !CaptureEnemies(World, MatchSnapshot, OutError) || !ValidateSnapshot(MatchSnapshot, OutError))
	{
		return false;
	}

	OutSnapshot = MoveTemp(MatchSnapshot);
	return true;
}

bool UWeaponSnapshotCoordinator::CapturePlayers(const UWorld* World, FWeaponSnapshot& Snapshot, FString& OutError) const
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
		Player.PlayerId = PlayerState->GetPlayerName();
		Player.PlayerName = PlayerState->GetPlayerName();	/* TODO: Determine whether to keep Player Name */
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
		Player.Rotation = Character->GetActorRotation();
		Player.AimDirection = Character->GetAimDirection();
		Player.Velocity = Character->GetVelocity();
		if (!IsFiniteVector(Player.Location) || !IsFiniteVector(Player.AimDirection) || !IsFiniteVector(Player.Velocity))
		{
			OutError = FString::Printf(TEXT("Player %s Location / AimDirection / Velocity includes NaN or Infinity"), *Player.PlayerName);
			return false;
		}
	}

	return true;
}

bool UWeaponSnapshotCoordinator::CaptureEnemies(const UWorld* World, FWeaponSnapshot& Snapshot, FString& OutError)
{
	for (TActorIterator<AEnemyCharacter> It(World); It; ++It)
	{
		const AEnemyCharacter* Enemy = *It;
		FWeaponEnemySnapshot& EnemySnapshot = Snapshot.Enemies.AddDefaulted_GetRef();
		EnemySnapshot.EnemyClass = FSoftClassPath(Enemy->GetClass());
		EnemySnapshot.Location = Enemy->GetActorLocation();
		EnemySnapshot.Rotation = Enemy->GetActorRotation();
		EnemySnapshot.Velocity = Enemy->GetVelocity();
		EnemySnapshot.AimDirection = Enemy->GetActorForwardVector();
		EnemySnapshot.EnemyConfigRowName = Enemy->GetEnemyConfigRowName();
		EnemySnapshot.AttributeState.Values = CaptureAttributeValues(Enemy->GetAbilitySystemComponent());
		if (!IsFiniteVector(EnemySnapshot.Location) || !IsFiniteVector(EnemySnapshot.Velocity))
		{
			OutError = FString::Printf(TEXT("Enemy %s Location / Velocity includes NaN or Infinity"), *Enemy->GetName());
			return false;
		}
	}
	return true;
}

TMap<FGameplayTag, float> UWeaponSnapshotCoordinator::CaptureAttributeValues(const UAbilitySystemComponent* ASC)
{
	TMap<FGameplayTag, float> Result;
	if (!ASC)
	{
		return Result;
	}
	for (const auto& [Tag, Attr] : UAttributeModifierManager::GetAttributeTagMap())
	{
		Result.Add(Tag, ASC->GetNumericAttribute(Attr));
	}
	return Result;
}

void UWeaponSnapshotCoordinator::RestoreAttributeValues(UAbilitySystemComponent* ASC, const TMap<FGameplayTag, float>& Values)
{
	if (!ASC)
	{
		return;
	}
	for (const auto& [Tag, Value] : Values)
	{
		if (const FGameplayAttribute* Attr = UAttributeModifierManager::GetAttributeTagMap().Find(Tag))
		{
			ASC->SetNumericAttributeBase(*Attr, Value);
		}
	}
}


bool UWeaponSnapshotCoordinator::ValidateSnapshot(const FWeaponSnapshot& Snapshot, FString& OutError)
{
	OutError.Reset();
	if (Snapshot.SchemaVersion != WeaponSnapshotVersion || Snapshot.Revision <= 0 || !Snapshot.MatchId.IsValid())
	{
		OutError = TEXT("Snapshot version, identity or revision is invalid");
		return false;
	}

	if (Snapshot.MaxPlayers <= 0 || Snapshot.Players.Num() > Snapshot.MaxPlayers || Snapshot.MapPackageName.IsEmpty())
	{
		OutError = TEXT("Snapshot player capacity or map package name is invalid");
		return false;
	}

	TSet<FString> PlayerIds;
	TSet<int32> Slots;
	for (const FWeaponPlayerSnapshot& Player : Snapshot.Players)
	{
		if (Player.PlayerId.IsEmpty() || PlayerIds.Contains(Player.PlayerId) || Slots.Contains(Player.SlotIndex))
		{
			OutError = TEXT("Snapshot has duplicated or invalid player ID/Slot");
			return false;
		}
		if (Player.SlotIndex < 0 || Player.SlotIndex >= Snapshot.MaxPlayers)
		{
			OutError = TEXT("Snapshot has player with out-of-bounds Slot");
			return false;
		}
		if (Player.bHasPawn && (!IsFiniteVector(Player.Location) || !IsFiniteVector(Player.AimDirection) || !IsFiniteVector(Player.Velocity)))
		{
			OutError = TEXT("Snapshot has player Pawn state containing NaN or infinity");
			return false;
		}

		PlayerIds.Add(Player.PlayerId);
		Slots.Add(Player.SlotIndex);
	}

	for (const FWeaponEnemySnapshot& Enemy : Snapshot.Enemies)
	{
		if (!Enemy.EnemyClass.IsValid() || !IsFiniteVector(Enemy.Location) || !IsFiniteVector(Enemy.Velocity) || !IsFiniteVector(Enemy.AimDirection))
		{
			OutError = TEXT("Snapshot has invalid enemy class or movement state");
			return false;
		}
		for (const auto& [Tag, Value] : Enemy.AttributeState.Values)
		{
			if (!FMath::IsFinite(Value))
			{
				OutError = FString::Printf(TEXT("Enemy attribute %s contains NaN or infinity"), *Tag.ToString());
				return false;
			}
		}
	}

	return true;
}

bool UWeaponSnapshotCoordinator::RestoreSnapshot(const FWeaponSnapshot& Snapshot, FString& OutError)
{
	OutError.Reset();
	if (!ValidateSnapshot(Snapshot, OutError))
	{
		return false;
	}

	const UWorld* World = GetWorld();
	if (!World || World->GetNetMode() == NM_Client)
	{
		OutError = TEXT("Snapshot could only be restored from valid World on server");
		return false;
	}

	AWeaponGameState* GameState = World->GetGameState<AWeaponGameState>();
	if (!GameState)
	{
		OutError = TEXT("Current World does not have AWeaponGameState");
		return false;
	}

	GameState->SetMatchPhase(Snapshot.MatchPhase, Snapshot.RemainingPhaseTime);

	if (!RestorePlayers(Snapshot, OutError) || !RestoreEnemies(Snapshot, OutError))
	{
		return false;
	}

	return true;
}

bool UWeaponSnapshotCoordinator::RestorePlayers(const FWeaponSnapshot& Snapshot, FString& OutError) const
{
	const UWorld* World = GetWorld();
	AWeaponGameState* GameState = World->GetGameState<AWeaponGameState>();
	AWeaponGameMode* GameMode = World->GetAuthGameMode<AWeaponGameMode>();

	for (APlayerState* BasePlayerState : GameState->PlayerArray)
	{
		AWeaponPlayerState* PlayerState = Cast<AWeaponPlayerState>(BasePlayerState);
		if (!PlayerState)
		{
			continue;
		}

		const FWeaponPlayerSnapshot* Snap = Snapshot.Players.FindByPredicate(
			[&](const FWeaponPlayerSnapshot& P) { return P.PlayerId == PlayerState->GetPlayerName(); });
		if (!Snap)
		{
			continue;
		}

		PlayerState->SetPlayerReady(Snap->bIsReady);
		PlayerState->SetTeamId(Snap->TeamId);

		if (Snap->bHasPawn)
		{
			APlayerController* PlayerController = PlayerState->GetPlayerController();
			if (!PlayerController)
			{
				continue;
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

			if (Character)
			{
				Character->SetActorLocation(Snap->Location);
				Character->SetActorRotation(Snap->Rotation);
			}
		}

		RestoreAttributeValues(PlayerState->GetAbilitySystemComponent(), Snap->AttributeState.Values);
	}

	return true;
}

bool UWeaponSnapshotCoordinator::RestoreEnemies(const FWeaponSnapshot& Snapshot, FString& OutError) const
{
	UWorld* World = GetWorld();

	TArray<AEnemyCharacter*> ExistingEnemies;
	for (TActorIterator<AEnemyCharacter> It(World); It; ++It)
	{
		ExistingEnemies.Add(*It);
	}
	for (AEnemyCharacter* Enemy : ExistingEnemies)
	{
		Enemy->Destroy();
	}

	for (const FWeaponEnemySnapshot& EnemySnap : Snapshot.Enemies)
	{
		UClass* EnemyClass = EnemySnap.EnemyClass.TryLoadClass<AEnemyCharacter>();
		if (!EnemyClass)
		{
			continue;
		}

		FActorSpawnParameters Params;
		Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AdjustIfPossibleButAlwaysSpawn;

		if (AEnemyCharacter* Enemy = World->SpawnActor<AEnemyCharacter>(EnemyClass, EnemySnap.Location, EnemySnap.Rotation, Params))
		{
			RestoreAttributeValues(Enemy->GetAbilitySystemComponent(), EnemySnap.AttributeState.Values);

			if (!IsFiniteVector(Enemy->GetActorLocation()))
			{
				OutError = TEXT("Restored enemy at infinite location");
				return false;
			}
		}
	}

	return true;
}

bool UWeaponSnapshotCoordinator::IsFiniteVector(const FVector& Value)
{
	return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
}
