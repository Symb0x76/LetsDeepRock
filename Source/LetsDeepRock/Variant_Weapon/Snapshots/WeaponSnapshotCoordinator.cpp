#include "WeaponSnapshotCoordinator.h"

#include "Engine/World.h"
#include "GameFramework/GameStateBase.h"
#include "WeaponGameState.h"
#include "WeaponPlayerState.h"
#include "WeaponCharacter.h"
#include "WeaponNPC.h"
#include "WeaponMatchSnapshot.h"
#include "WeaponSnapshotRegistry.h"
#include "EngineUtils.h"
#include "Engine/GameInstance.h"
#include "UObject/Package.h"

bool UWeaponSnapshotCoordinator::ShouldCreateSubsystem(UObject* Outer) const
{
	const UGameInstance* GameInstance = Cast<UGameInstance>(Outer);
	return GameInstance && GameInstance->GetWorld() && GameInstance->GetWorld()->GetNetMode() != NM_Client;
}

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
	MatchSnapshot.SnapshotId = FGuid::NewGuid();
	if (!ActiveMatchId.IsValid())
	{
		ActiveMatchId = FGuid::NewGuid();
	}
	MatchSnapshot.MatchId = ActiveMatchId;
	MatchSnapshot.SnapshotRevision = NextSnapshotRevision++;
	MatchSnapshot.MapPackageName = World->GetOutermost()->GetName();
	MatchSnapshot.MatchPhase = GameState->GetMatchPhase();
	MatchSnapshot.ServerWorldTimeSeconds = GameState->GetServerWorldTimeSeconds();
	MatchSnapshot.RemainingPhaseTime = GameState->GetRemainingPhaseTime();
	MatchSnapshot.MaxPlayers = GameState->GetMaxPlayers();
	MatchSnapshot.CapturedAtUtc = FDateTime::UtcNow();

	if (!FMath::IsFinite(MatchSnapshot.ServerWorldTimeSeconds) || !FMath::IsFinite(MatchSnapshot.RemainingPhaseTime))
	{
		OutError = TEXT("GameState time is not valid");
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
		Player.StablePlayerId.Kind = EWeaponStableEntityKind::Player;
		Player.StablePlayerId.Namespace = TEXT("PlayerState");
		Player.StablePlayerId.Value = PlayerState->GetPlayerName();
		Player.PlayerName = PlayerState->GetPlayerName();
		Player.SlotIndex = PlayerState->GetPlayerSlotIndex();
		Player.bIsReady = PlayerState->GetIsPlayerReady();

		const AWeaponCharacter* Character = Cast<AWeaponCharacter>(PlayerState->GetPawn());
		if (!Character)
		{
			continue;
		}

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
	const UWeaponSnapshotRegistry* Registry = World->GetSubsystem<UWeaponSnapshotRegistry>();
	if (!Registry)
	{
		OutError = TEXT("Server World does not have Stable ID Registry");
		return false;
	}

	for (TActorIterator<AWeaponNPC> It(World); It; ++It)
	{
		const AWeaponNPC* Enemy = *It;
		FWeaponStableEntityId StableEnemyId;
		if (!Registry->FindId(Enemy, StableEnemyId))
		{
			OutError = FString::Printf(TEXT("Enemy %s has not registered Stable ID"), *Enemy->GetName());
			return false;
		}

		FWeaponEnemySnapshot& EnemySnapshot = Snapshot.World.LivingEnemies.AddDefaulted_GetRef();
		EnemySnapshot.StableEnemyId = StableEnemyId;
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
	if (Snapshot.SchemaVersion != WeaponMatchSnapshotVersion || !Snapshot.SnapshotId.IsValid() || Snapshot.SnapshotRevision <= 0 || !Snapshot.MatchId.IsValid())
	{
		OutError = TEXT("Snapshot version, identity or revision is invalid");
		return false;
	}

	if (Snapshot.MaxPlayers <= 0 || Snapshot.Players.Num() > Snapshot.MaxPlayers || Snapshot.MapPackageName.IsEmpty())
	{
		OutError = TEXT("Snapshot player capacity or map package name is invalid");
		return false;
	}

	TSet<FWeaponStableEntityId> StableIds;
	TSet<int32> Slots;
	for (const FWeaponPlayerSnapshot& Player : Snapshot.Players)
	{
		if (!Player.StablePlayerId.IsValid() || StableIds.Contains(Player.StablePlayerId) || Slots.Contains(Player.SlotIndex))
		{
			OutError = TEXT("Snapshot has duplicated or invalid player Stable ID/Slot");
			return false;
		}
		if (Player.SlotIndex < 0 || Player.SlotIndex >= Snapshot.MaxPlayers)
		{
			OutError = TEXT("Snapshot has player with out-of-bounds Slot");
			return false;
		}
		StableIds.Add(Player.StablePlayerId);
		Slots.Add(Player.SlotIndex);
	}

	for (const FWeaponGrenadeProjectileSnapshot& Grenade : Snapshot.World.ActiveGrenades)
	{
		if (!Grenade.Projectile.StableProjectileId.IsValid() || Grenade.bHasExploded || !FMath::IsFinite(Grenade.RemainingFuseSeconds) || Grenade.RemainingFuseSeconds < 0.0f)
		{
			OutError = TEXT("Snapshot has invalid active grenade");
			return false;
		}
	}

	return true;
}

bool UWeaponSnapshotCoordinator::IsFiniteVector(const FVector& Value)
{
	return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
}
