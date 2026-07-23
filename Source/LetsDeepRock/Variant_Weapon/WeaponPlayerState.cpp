// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponPlayerState.h"
#include "AbilitySystemComponent.h"
#include "WeaponGameMode.h"
#include "WeaponGameState.h"
#include "Attributes/WeaponHealthSet.h"
#include "Attributes/WeaponCombatSet.h"
#include "Attributes/WeaponMovementSet.h"
#include "Attributes/WeaponResistanceSet.h"
#include "Attributes/WeaponResourceSet.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"

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

// 声明类成员同步 (Server -> Client)
void AWeaponPlayerState::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	// 保留父类复制规则
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	
	// 注册子类复制规则
	DOREPLIFETIME(AWeaponPlayerState, LobbyState);
	DOREPLIFETIME(AWeaponPlayerState, TeamId);
}

void AWeaponPlayerState::SetTeamId(int32 InTeamId)
{
	if (HasAuthority())
	{
		TeamId = InTeamId;
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
	if (!WeaponGameState ||
		(WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::Waiting &&
		 WeaponGameState->GetMatchPhase() != EWeaponMatchPhase::Starting))
	{
		return;
	}

	SetPlayerReady(bNewIsReady);
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
}

void AWeaponPlayerState::OnRep_LobbyState(const FWeaponPlayerLobbyState OldState)
{
	OnLobbyStateChanged.Broadcast(OldState, LobbyState);
}
