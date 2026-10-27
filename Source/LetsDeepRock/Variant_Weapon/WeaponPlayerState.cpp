// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponPlayerState.h"
#include "AbilitySystemComponent.h"
#include "WeaponGameMode.h"
#include "WeaponGameState.h"
#include "WeaponCharacter.h"
#include "Attributes/WeaponHealthSet.h"
#include "Attributes/WeaponCombatSet.h"
#include "Attributes/WeaponMovementSet.h"
#include "Attributes/WeaponResistanceSet.h"
#include "Attributes/WeaponResourceSet.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "Data/ProfessionSaveGame.h"

AWeaponPlayerState::AWeaponPlayerState()
{
	// 在构造函数中创建 ASC 并标记复制（GAS 要求 ASC 在 C++ 构造时创建）
	AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);

	// 玩家 ASC 推荐 Mixed 复制模式：
	// GameplayEffect 只复制给拥有它的客户端，GameplayTags/Cues 复制给所有人
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Mixed);

	// 创建生命值属性集（隶属于同一 Owner = PlayerState）
	HealthSet = CreateDefaultSubobject<UWeaponHealthSet>(TEXT("HealthSet"));

	// 创建战斗属性集
	CombatSet = CreateDefaultSubobject<UWeaponCombatSet>(TEXT("CombatSet"));

	// 创建移动属性集
	MovementSet = CreateDefaultSubobject<UWeaponMovementSet>(TEXT("MovementSet"));

	// 创建抗性属性集
	ResistanceSet = CreateDefaultSubobject<UWeaponResistanceSet>(TEXT("ResistanceSet"));

	// 创建资源属性集
	ResourceSet = CreateDefaultSubobject<UWeaponResourceSet>(TEXT("ResourceSet"));

	// ASC 在 PlayerState 上时需要更高复制频率，避免属性/标签同步延迟
	SetNetUpdateFrequency(100.0f);
}

UAbilitySystemComponent* AWeaponPlayerState::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

void AWeaponPlayerState::OnDeactivated()
{
	if (AWeaponGameMode* WeaponGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AWeaponGameMode>() : nullptr;
		WeaponGameMode && WeaponGameMode->HandleAttributionPlayerStateDeactivated(this))
	{
		return;
	}
	Super::OnDeactivated();
}

// 声明类成员同步 (Server -> Client)
void AWeaponPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	// 保留父类复制规则
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	
	// 注册子类复制规则
	DOREPLIFETIME(AWeaponPlayerState, LobbyState);
	DOREPLIFETIME(AWeaponPlayerState, TeamId);
	DOREPLIFETIME(AWeaponPlayerState, ProfessionLoadout);
	DOREPLIFETIME(AWeaponPlayerState, RespawnServerTime);
	DOREPLIFETIME(AWeaponPlayerState, DownedServerTime);
	DOREPLIFETIME(AWeaponPlayerState, LifeState);
}

void AWeaponPlayerState::CopyProperties(APlayerState* PlayerState)
{
	Super::CopyProperties(PlayerState);

	if (AWeaponPlayerState* NewWeaponPlayerState = Cast<AWeaponPlayerState>(PlayerState))
	{
		NewWeaponPlayerState->LobbyState = LobbyState;
		NewWeaponPlayerState->TeamId = TeamId;
		NewWeaponPlayerState->ProfessionLoadout = ProfessionLoadout;
	}
}

bool AWeaponPlayerState::SetProfessionLoadout(FName ProfessionId, FName PrimaryWeaponId, FName SecondaryWeaponId)
{
	if (!HasAuthority() || !UProfessionSaveGame::IsProfessionSelectionValid(this, ProfessionId, PrimaryWeaponId, SecondaryWeaponId))
	{
		return false;
	}

	ProfessionLoadout.ProfessionId = ProfessionId;
	ProfessionLoadout.PrimaryWeaponId = PrimaryWeaponId;
	ProfessionLoadout.SecondaryWeaponId = SecondaryWeaponId;
	ForceNetUpdate();
	return true;
}

void AWeaponPlayerState::OnRep_ProfessionLoadout()
{
	if (AWeaponCharacter* Character = GetPawn<AWeaponCharacter>())
	{
		Character->RefreshProfessionLoadout();
	}
}

void AWeaponPlayerState::SetTeamId(int32 InTeamId)
{
	if (HasAuthority())
	{
		TeamId = InTeamId;
	}
}

void AWeaponPlayerState::SetRespawnServerTime(const float NewRespawnServerTime)
{
	if (HasAuthority() && RespawnServerTime != NewRespawnServerTime)
	{
		RespawnServerTime = NewRespawnServerTime;
		ForceNetUpdate();
	}
}

void AWeaponPlayerState::SetDownedServerTime(const float NewDownedServerTime)
{
	if (HasAuthority() && DownedServerTime != NewDownedServerTime)
	{
		DownedServerTime = NewDownedServerTime;
		ForceNetUpdate();
	}
}

void AWeaponPlayerState::SetLifeStateAuthority(const EWeaponPlayerLifeState NewLifeState)
{
	if (HasAuthority() && LifeState != NewLifeState)
	{
		LifeState = NewLifeState;
		ForceNetUpdate();
	}
}

void AWeaponPlayerState::SetPlayerSlotIndex(const int32 NewSlotIndex)
{
	if (!HasAuthority())
	{
		return;
	}

	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	const int32 MaxPlayers = WeaponGameState ? WeaponGameState->GetMaxPlayers() : 4;

	if (NewSlotIndex != INDEX_NONE && (NewSlotIndex < 0 || NewSlotIndex >= MaxPlayers))
	{
		UE_LOG(LogTemp, Warning, TEXT("Player slot must be INDEX_NONE or between 0~%d."), MaxPlayers - 1);
		return;
	}
	
	if (LobbyState.SlotIndex == NewSlotIndex)
	{
		return;
	}
	
	const FWeaponPlayerLobbyState OldState = LobbyState;
	LobbyState.SlotIndex = NewSlotIndex;
	
	// 服务端不会自动调用 OnRep，主动通知主机本地 UI。
	OnLobbyStateChanged.Broadcast(OldState, LobbyState);
	ForceNetUpdate();

	if (AWeaponGameMode* WeaponGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AWeaponGameMode>() : nullptr)
	{
		WeaponGameMode->HandlePlayerReadyChanged();
	}
}

void AWeaponPlayerState::RequestSetPlayerReady(const bool bNewIsReady)
{
	if (HasAuthority())
	{
		ApplyPlayerReadyRequest(bNewIsReady);
	}
	else
	{
		ServerSetPlayerReady(bNewIsReady);
	}
}

void AWeaponPlayerState::ServerSetPlayerReady_Implementation(const bool bNewIsReady)
{
	ApplyPlayerReadyRequest(bNewIsReady);
}

void AWeaponPlayerState::ApplyPlayerReadyRequest(const bool bNewIsReady)
{
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!WeaponGameState || WeaponGameState->IsGameplayRecoveryBlocked() ||
		(WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::Waiting &&
		 WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::Starting))
	{
		return;
	}

	SetPlayerReady(bNewIsReady);
}

void AWeaponPlayerState::RequestSetMissionSettings(const int32 Seed, const int32 Difficulty)
{
	if (HasAuthority())
	{
		ApplyMissionSettingsRequest(Seed, Difficulty);
	}
	else
	{
		ServerSetMissionSettings(Seed, Difficulty);
	}
}

void AWeaponPlayerState::ServerSetMissionSettings_Implementation(const int32 Seed, const int32 Difficulty)
{
	ApplyMissionSettingsRequest(Seed, Difficulty);
}

void AWeaponPlayerState::ApplyMissionSettingsRequest(const int32 Seed, const int32 Difficulty)
{
	// 任务设置只允许 Listen Host / Standalone 的本地玩家（房主）修改：
	// Server RPC 上下文中，发起者的 PlayerState 就是 this；房主拥有世界上第一个本地 PC。
	AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	const APlayerController* HostController = GetWorld() ? GetWorld()->GetFirstPlayerController() : nullptr;
	if (!HasAuthority() || !WeaponGameState || !HostController || !HostController->IsLocalController() ||
		HostController->PlayerState != this)
	{
		return;
	}

	FWeaponMissionSettings NewSettings;
	NewSettings.Seed = FMath::Max(Seed, 0);
	NewSettings.Difficulty = FMath::Clamp(Difficulty, 1, 3);
	WeaponGameState->SetMissionSettingsAuthority(NewSettings);
}

void AWeaponPlayerState::SetPlayerReady(const bool bNewIsReady)
{
	if (!HasAuthority())
	{
		return;
	}

	if (LobbyState.bIsReady == bNewIsReady)
	{
		return;
	}

	const FWeaponPlayerLobbyState OldState = LobbyState;
	LobbyState.bIsReady = bNewIsReady;

	OnLobbyStateChanged.Broadcast(OldState, LobbyState);
	ForceNetUpdate();

	if (AWeaponGameMode* WeaponGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AWeaponGameMode>() : nullptr)
	{
		WeaponGameMode->HandlePlayerReadyChanged();
	}
}

void AWeaponPlayerState::OnRep_LobbyState(const FWeaponPlayerLobbyState OldState)
{
	OnLobbyStateChanged.Broadcast(OldState, LobbyState);
}
