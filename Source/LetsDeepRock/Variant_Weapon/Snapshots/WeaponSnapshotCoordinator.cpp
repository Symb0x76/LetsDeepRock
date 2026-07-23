#include "WeaponSnapshotCoordinator.h"

#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "WeaponGameState.h"
#include "WeaponPlayerState.h"
#include "WeaponCharacter.h"
#include "WeaponNPC.h"
#include "WeaponMatchSnapshot.h"
#include "EngineUtils.h"
#include "UObject/Package.h"

bool UWeaponSnapshotCoordinator::CaptureSnapshot(UWorld* World, FWeaponMatchSnapshot& OutSnapshot, FString& OutError)
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

	FWeaponMatchSnapshot MatchSnapshot;
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

	if (!CapturePlayers(World, MatchSnapshot, OutError) || !CaptureNPCs(World, MatchSnapshot, OutError) || !ValidateSnapshot(MatchSnapshot, OutError))
	{
		return false;
	}

	OutSnapshot = MoveTemp(MatchSnapshot);
	return true;
}

bool UWeaponSnapshotCoordinator::CapturePlayers(const UWorld* World, FWeaponMatchSnapshot& Snapshot, FString& OutError) const
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
		Player.PlayerName = PlayerState->GetPlayerName();
		Player.SlotIndex = PlayerState->GetPlayerSlotIndex();
		Player.bIsReady = PlayerState->GetIsPlayerReady();

		const AWeaponCharacter* Character = Cast<AWeaponCharacter>(PlayerState->GetPawn());
		if (!Character)
		{
			continue;
		}

		Player.bHasPawn = true;
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

bool UWeaponSnapshotCoordinator::CaptureNPCs(const UWorld* World, FWeaponMatchSnapshot& Snapshot, FString& OutError) const
{
	for (TActorIterator<AWeaponNPC> It(World); It; ++It)
	{
		const AWeaponNPC* Enemy = *It;
		FWeaponEnemySnapshot& EnemySnapshot = Snapshot.Enemies.AddDefaulted_GetRef();
		EnemySnapshot.EnemyClass = FSoftClassPath(Enemy->GetClass());
		EnemySnapshot.Location = Enemy->GetActorLocation();
		EnemySnapshot.Rotation = Enemy->GetActorRotation();
		EnemySnapshot.Velocity = Enemy->GetVelocity();
		if (!IsFiniteVector(EnemySnapshot.Location) || !IsFiniteVector(EnemySnapshot.Velocity))
		{
			OutError = FString::Printf(TEXT("Enemy %s Location / Velocity includes NaN or Infinity"), *Enemy->GetName());
			return false;
		}
	}
	return true;
}

bool UWeaponSnapshotCoordinator::ValidateSnapshot(const FWeaponMatchSnapshot& Snapshot, FString& OutError) const
{
	OutError.Reset();
	if (Snapshot.SchemaVersion != WeaponMatchSnapshotVersion || Snapshot.Revision <= 0 || !Snapshot.MatchId.IsValid())
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
			OutError = TEXT("Snapshot has player Pawn state containing NaN or Infinity");
			return false;
		}

		PlayerIds.Add(Player.PlayerId);
		Slots.Add(Player.SlotIndex);
	}

	for (const FWeaponEnemySnapshot& Enemy : Snapshot.Enemies)
	{
		if (!Enemy.EnemyClass.IsValid() || !IsFiniteVector(Enemy.Location) || !IsFiniteVector(Enemy.Velocity))
		{
			OutError = TEXT("Snapshot has invalid enemy class or movement state");
			return false;
		}
	}

	return true;
}

bool UWeaponSnapshotCoordinator::IsFiniteVector(const FVector& Value)
{
	return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
}
