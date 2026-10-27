/**
 * @Author: Symb0x76 9667434@qq.com
 * @Date: 2026-07-13 12:19:47
 * @LastEditors: Symb0x76 9667434@qq.com
 * @LastEditTime: 2026-07-13 12:56:38
 * @FilePath: Source/LetsDeepRock/Variant_Weapon/WeaponGameState.cpp
 */

#include "WeaponGameState.h"

#include "WeaponPlayerState.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

AWeaponGameState::AWeaponGameState()
{
	bReplicates = true;	// 显式设置 GameStateBase 可复制
}

int32 AWeaponGameState::GetConnectedPlayerCount() const
{
	return PlayerArray.Num();
}

bool AWeaponGameState::IsGameplayRecoveryBlockedFor(const UObject* WorldContextObject)
{
	const UWorld* World = WorldContextObject ? WorldContextObject->GetWorld() : nullptr;
	const AWeaponGameState* WeaponGameState = World ? World->GetGameState<AWeaponGameState>() : nullptr;
	return WeaponGameState && WeaponGameState->IsGameplayRecoveryBlocked();
}

bool AWeaponGameState::SetGameplayRecoveryBlockedAuthority(const bool bBlocked)
{
	if (!HasAuthority())
	{
		return false;
	}
	if (bGameplayRecoveryBlocked == bBlocked)
	{
		return true;
	}

	bGameplayRecoveryBlocked = bBlocked;
	OnGameplayRecoveryBarrierChanged.Broadcast(bGameplayRecoveryBlocked);
	ForceNetUpdate();
	return true;
}

bool AWeaponGameState::SetMaxPlayersAuthority(const int32 NewMaxPlayers)
{
	if (!HasAuthority() || NewMaxPlayers < 1 || NewMaxPlayers > 4 || PlayerArray.Num() > NewMaxPlayers)
	{
		return false;
	}
	for (const APlayerState* PlayerState : PlayerArray)
	{
		if (const AWeaponPlayerState* WeaponPlayerState = Cast<AWeaponPlayerState>(PlayerState); WeaponPlayerState && WeaponPlayerState->GetPlayerSlotIndex() >= NewMaxPlayers)
		{
			return false;
		}
	}

	MaxPlayers = NewMaxPlayers;
	ForceNetUpdate();
	return true;
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

void AWeaponGameState::OnRep_MissionSettings() const
{
	// 任务设置由 Lobby 4Hz 轮询刷新展示，无需额外广播。
}

void AWeaponGameState::OnRep_GameplayRecoveryBlocked() const
{
	OnGameplayRecoveryBarrierChanged.Broadcast(bGameplayRecoveryBlocked);
}

void AWeaponGameState::OnRep_ObjectiveState() const
{
	// 目标/撤离进度由 HUD 轮询读取,无需广播。
}

void AWeaponGameState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWeaponGameState, MaxPlayers);
	DOREPLIFETIME(AWeaponGameState, MatchState);
	DOREPLIFETIME(AWeaponGameState, MissionSettings);
	DOREPLIFETIME(AWeaponGameState, bGameplayRecoveryBlocked);
	DOREPLIFETIME(AWeaponGameState, ObjectiveState);
	DOREPLIFETIME(AWeaponGameState, ReinforcementPool);
	DOREPLIFETIME(AWeaponGameState, ReviveRadius);
}

void AWeaponGameState::SetReviveRadiusAuthority(const float NewRadius)
{
	if (!HasAuthority())
	{
		return;
	}
	const float Clamped = FMath::Max(NewRadius, 0.0f);
	if (!FMath::IsNearlyEqual(ReviveRadius, Clamped))
	{
		ReviveRadius = Clamped;
		ForceNetUpdate();
	}
}

void AWeaponGameState::SetReinforcementPoolAuthority(const int32 NewPool)
{
	if (!HasAuthority())
	{
		return;
	}
	const int32 Clamped = FMath::Max(NewPool, 0);
	if (ReinforcementPool != Clamped)
	{
		ReinforcementPool = Clamped;
		ForceNetUpdate();
	}
}

void AWeaponGameState::RestoreReinforcementPoolForRecovery(const int32 RestoredPool)
{
	SetReinforcementPoolAuthority(RestoredPool);
}

void AWeaponGameState::SetMissionOutcomeAuthority(const EWeaponMissionOutcome NewOutcome)
{
	if (!HasAuthority() || MatchState.Outcome == NewOutcome)
	{
		return;
	}
	MatchState.Outcome = NewOutcome;
	ForceNetUpdate();
}

void AWeaponGameState::SetObjectiveStateAuthority(const FWeaponObjectiveState& NewState)
{
	if (!HasAuthority())
	{
		return;
	}
	ObjectiveState = NewState;
}

void AWeaponGameState::RestoreMissionSettingsForRecovery(const FWeaponMissionSettings& RestoredSettings)
{
	if (!HasAuthority())
	{
		return;
	}
	MissionSettings.Seed = FMath::Max(RestoredSettings.Seed, 0);
	MissionSettings.Difficulty = FMath::Clamp(RestoredSettings.Difficulty, 1, 3);
}

bool AWeaponGameState::SetMissionSettingsAuthority(const FWeaponMissionSettings& NewSettings)
{
	if (!HasAuthority() || IsGameplayRecoveryBlocked() ||
		(GetMatchPhase() != EWeaponMatchPhase::Waiting && GetMatchPhase() != EWeaponMatchPhase::Starting))
	{
		return false;
	}
	if (MissionSettings.Seed == NewSettings.Seed && MissionSettings.Difficulty == NewSettings.Difficulty)
	{
		return true;
	}
	MissionSettings = NewSettings;
	return true;
}
