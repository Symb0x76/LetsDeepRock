// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponReinforcementRules.h"
#include "Engine/World.h"
#include "GameFramework/Controller.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/PlayerState.h"
#include "WeaponPlayerState.h"

int32 WeaponReinforcementPoolForDifficulty(int32 Difficulty, const TArray<int32>& PoolByDifficulty)
{
	const int32 Index = FMath::Clamp(Difficulty, 1, 3) - 1;
	const int32 Value = PoolByDifficulty.Num() >= 3
		? PoolByDifficulty[Index]
		: WeaponReinforcementDefaults::PoolByDifficulty[Index];
	return FMath::Max(0, Value);
}

bool WeaponShouldEnterDowned(int32 OtherUpPlayers, bool bDownedRequiresReviver)
{
	return !bDownedRequiresReviver || OtherUpPlayers > 0;
}

float WeaponDownedDamageToSeconds(float Damage, float MaxHealth, float BleedOutSeconds)
{
	if (MaxHealth <= 0.f)
	{
		return FMath::Max(0.f, BleedOutSeconds);
	}
	return FMath::Max(0.f, Damage / MaxHealth * BleedOutSeconds);
}

bool WeaponShouldFailMission(int32 Pool, int32 UpPlayers, int32 ConnectedPlayers)
{
	return ConnectedPlayers > 0 && Pool <= 0 && UpPlayers <= 0;
}

bool FWeaponReviveTracker::Advance(const UObject* Target, bool bHeld, float DeltaTime, float ReviveSeconds)
{
	if (!Target || !bHeld)
	{
		Reset();
		return false;
	}
	if (TargetPtr.Get() != Target)
	{
		TargetPtr = Target;
		Elapsed = 0.f;
	}
	Elapsed += FMath::Max(0.f, DeltaTime);
	if (Elapsed >= ReviveSeconds)
	{
		Reset();
		return true;
	}
	return false;
}

float FWeaponReviveTracker::GetProgress(float ReviveSeconds) const
{
	return ReviveSeconds > 0.f ? FMath::Clamp(Elapsed / ReviveSeconds, 0.f, 1.f) : 0.f;
}

void FWeaponReviveTracker::Reset()
{
	TargetPtr = nullptr;
	Elapsed = 0.f;
}

EWeaponPlayerLifeState WeaponGetLifeState(const APlayerState* PlayerState)
{
	const AWeaponPlayerState* WeaponPlayerState = Cast<AWeaponPlayerState>(PlayerState);
	return WeaponPlayerState ? WeaponPlayerState->GetLifeState() : EWeaponPlayerLifeState::Dead;
}

int32 WeaponCountUpPlayers(const UWorld* World, const AController* Exclude)
{
	const AGameStateBase* GameState = World ? World->GetGameState() : nullptr;
	if (!GameState)
	{
		return 0;
	}

	int32 Count = 0;
	for (const APlayerState* PlayerState : GameState->PlayerArray)
	{
		if (!PlayerState || (Exclude && PlayerState->GetOwningController() == Exclude))
		{
			continue;
		}
		if (WeaponGetLifeState(PlayerState) == EWeaponPlayerLifeState::Up && PlayerState->GetPawn())
		{
			++Count;
		}
	}
	return Count;
}

int32 WeaponReinforceQueueRank(const TConstArrayView<TPair<float, int32>> PendingTimesAndIds, const int32 OwnId)
{
	const TPair<float, int32>* Own = nullptr;
	for (const TPair<float, int32>& Entry : PendingTimesAndIds)
	{
		if (Entry.Value == OwnId)
		{
			Own = &Entry;
			break;
		}
	}
	if (!Own)
	{
		return INDEX_NONE;
	}

	int32 Rank = 0;
	for (const TPair<float, int32>& Entry : PendingTimesAndIds)
	{
		if (&Entry != Own && (Entry.Key < Own->Key || (Entry.Key == Own->Key && Entry.Value < Own->Value)))
		{
			++Rank;
		}
	}
	return Rank;
}
