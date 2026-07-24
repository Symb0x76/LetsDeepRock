/**
 * @Author: Symb0x76 9667434@qq.com
 * @Date: 2026-07-13 12:19:47
 * @LastEditors: Symb0x76 9667434@qq.com
 * @LastEditTime: 2026-07-13 12:56:38
 * @FilePath: Source/LetsDeepRock/Variant_Weapon/WeaponGameState.cpp
 */

#include "WeaponGameState.h"

#include "WeaponPlayerState.h"
#include "Net/UnrealNetwork.h"

AWeaponGameState::AWeaponGameState()
{
	bReplicates = true;	// 显式设置 GameStateBase 可复制
}

int32 AWeaponGameState::FindSmallestAvailablePlayerSlot() const
{
	TSet<int32> OccupiedSlots;

	for (const APlayerState* PlayerState : PlayerArray)
	{
		const AWeaponPlayerState* WeaponPlayerState = Cast<AWeaponPlayerState>(PlayerState);

		if (!WeaponPlayerState)
		{
			continue;
		}

		if (const int32 SlotIndex = WeaponPlayerState->GetPlayerSlotIndex(); 
			SlotIndex >= 0 && SlotIndex < MaxPlayers)
		{
			OccupiedSlots.Add(SlotIndex);
		}
	}

	for (int32 SlotIndex = 0; SlotIndex < MaxPlayers; ++SlotIndex)
	{
		if (!OccupiedSlots.Contains(SlotIndex))
		{
			return SlotIndex;
		}
	}

	return INDEX_NONE;
}

int32 AWeaponGameState::GetConnectedPlayerCount() const
{
	return PlayerArray.Num();
}

void AWeaponGameState::SetMatchPhase(const EWeaponMatchPhase NewPhase, const float PhaseDuration)
{
	if (!HasAuthority())
	{
		return;
	}

	MatchState.Phase = NewPhase;
	MatchState.PhaseEndServerTime = PhaseDuration > 0.0f ?
		GetServerWorldTimeSeconds() + PhaseDuration : 0.0f;
	
	// 服务端 UI 不会触发 OnRep，需要主动广播。
	OnMatchPhaseChanged.Broadcast(MatchState.Phase);
	
	// 尽快推送给所有客户端
	ForceNetUpdate();
}

float AWeaponGameState::GetRemainingPhaseTime() const
{
	return MatchState.PhaseEndServerTime > 0.0f ?
		FMath::Max(0.0f, MatchState.PhaseEndServerTime - GetServerWorldTimeSeconds()) : 0.0f;
}

void AWeaponGameState::OnRep_MatchState() const
{
	OnMatchPhaseChanged.Broadcast(MatchState.Phase);
}

void AWeaponGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWeaponGameState, MaxPlayers);
	DOREPLIFETIME(AWeaponGameState, MatchState);
}
