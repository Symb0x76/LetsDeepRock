// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponPlayerState.h"
#include "AbilitySystemComponent.h"
#include "Attributes/WeaponHealthSet.h"

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

	// ASC 在 PlayerState 上时需要更高的复制频率，避免属性/标签同步延迟
	SetNetUpdateFrequency(100.0f);
}

UAbilitySystemComponent* AWeaponPlayerState::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}
