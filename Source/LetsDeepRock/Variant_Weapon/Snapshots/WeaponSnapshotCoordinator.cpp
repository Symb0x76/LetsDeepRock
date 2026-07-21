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
		OutError = TEXT("Snapshot 只能由服务器从有效 World 捕获。");
		return false;
	}

	const AWeaponGameState* GameState = World->GetGameState<AWeaponGameState>();
	if (!GameState)
	{
		OutError = TEXT("当前 World 没有 AWeaponGameState。");
		return false;
	}

	FWeaponMatchSnapshot Candidate;
	Candidate.SnapshotId = FGuid::NewGuid();
	if (!ActiveMatchId.IsValid())
	{
		ActiveMatchId = FGuid::NewGuid();
	}
	Candidate.MatchId = ActiveMatchId;
	Candidate.SnapshotRevision = NextSnapshotRevision++;
	Candidate.MapPackageName = World->GetOutermost()->GetName();
	Candidate.MatchPhase = GameState->GetMatchPhase();
	Candidate.ServerWorldTimeSeconds = GameState->GetServerWorldTimeSeconds();
	Candidate.RemainingPhaseTime = GameState->GetRemainingPhaseTime();
	Candidate.MaxPlayers = GameState->GetMaxPlayers();
	Candidate.CapturedAtUtc = FDateTime::UtcNow();

	if (!FMath::IsFinite(Candidate.ServerWorldTimeSeconds) || !FMath::IsFinite(Candidate.RemainingPhaseTime))
	{
		OutError = TEXT("GameState 时间值不是有限数。");
		return false;
	}

	if (!CapturePlayers(World, Candidate, OutError)
		|| !CaptureNPCs(World, Candidate, OutError)
		|| !ValidateSnapshot(Candidate, OutError))
	{
		return false;
	}

	OutSnapshot = MoveTemp(Candidate);
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
			OutError = FString::Printf(TEXT("玩家 %s 的空间状态包含 NaN 或 Infinity。"), *Player.PlayerName);
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
		OutError = TEXT("服务器 World 没有 Stable ID Registry。");
		return false;
	}

	for (TActorIterator<AWeaponNPC> It(World); It; ++It)
	{
		const AWeaponNPC* Enemy = *It;
		FWeaponStableEntityId StableEnemyId;
		if (!Registry->FindId(Enemy, StableEnemyId))
		{
			OutError = FString::Printf(TEXT("Enemy %s 没有注册 Stable ID。"), *Enemy->GetName());
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
			OutError = FString::Printf(TEXT("Enemy %s location/velocity includes NaN or Infinity"), *Enemy->GetName());
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
		OutError = TEXT("Snapshot 版本、身份或 Revision 无效。");
		return false;
	}

	if (Snapshot.MaxPlayers <= 0 || Snapshot.Players.Num() > Snapshot.MaxPlayers || Snapshot.MapPackageName.IsEmpty())
	{
		OutError = TEXT("Snapshot 的玩家容量或地图包名无效。");
		return false;
	}

	TSet<FWeaponStableEntityId> StableIds;
	TSet<int32> Slots;
	for (const FWeaponPlayerSnapshot& Player : Snapshot.Players)
	{
		if (!Player.StablePlayerId.IsValid() || StableIds.Contains(Player.StablePlayerId) || Slots.Contains(Player.SlotIndex))
		{
			OutError = TEXT("Snapshot 中存在重复或无效的玩家 Stable ID/Slot。");
			return false;
		}
		if (Player.SlotIndex < 0 || Player.SlotIndex >= Snapshot.MaxPlayers)
		{
			OutError = TEXT("Snapshot 中存在越界的玩家 Slot。");
			return false;
		}
		StableIds.Add(Player.StablePlayerId);
		Slots.Add(Player.SlotIndex);
	}

	for (const FWeaponGrenadeProjectileSnapshot& Grenade : Snapshot.World.ActiveGrenades)
	{
		if (!Grenade.Projectile.StableProjectileId.IsValid() || Grenade.bHasExploded || !FMath::IsFinite(Grenade.RemainingFuseSeconds) || Grenade.RemainingFuseSeconds < 0.0f)
		{
			OutError = TEXT("Snapshot 中存在无效的活动手雷。");
			return false;
		}
	}

	return true;
}

bool UWeaponSnapshotCoordinator::IsFiniteVector(const FVector& Value)
{
	return FMath::IsFinite(Value.X) && FMath::IsFinite(Value.Y) && FMath::IsFinite(Value.Z);
}
