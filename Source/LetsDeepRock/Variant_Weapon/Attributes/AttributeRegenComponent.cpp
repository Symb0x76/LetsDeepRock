// Copyright Epic Games, Inc. All Rights Reserved.

#include "AttributeRegenComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "WeaponHealthSet.h"
#include "WeaponResourceSet.h"

UAttributeRegenComponent::UAttributeRegenComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// 降低 Tick 频率，不需要每帧（10Hz 已足够平滑）
	PrimaryComponentTick.TickInterval = 0.1f;
}

void UAttributeRegenComponent::BeginPlay()
{
	Super::BeginPlay();

	// 初始状态：允许立即恢复（延迟已归零）
	ShieldRegenTimer = 0.0f;
	bShieldRegenDelayed = false;
}

UAbilitySystemComponent* UAttributeRegenComponent::GetASC() const
{
	if (IAbilitySystemInterface* ASI = Cast<IAbilitySystemInterface>(GetOwner()))
	{
		return ASI->GetAbilitySystemComponent();
	}
	return nullptr;
}

void UAttributeRegenComponent::NotifyDamageTaken()
{
	// 受击时重置护盾再生延迟
	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC) return;

	const UWeaponHealthSet* HS = ASC->GetSet<UWeaponHealthSet>();
	if (HS)
	{
		ShieldRegenTimer = HS->GetShieldRegenDelay();
		bShieldRegenDelayed = true;
	}
}

void UAttributeRegenComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC) return;

	// 仅服务端写入属性
	if (!GetOwner()->HasAuthority())
	{
		return;
	}

	// --- 护盾再生 ---
	if (const UWeaponHealthSet* HS = ASC->GetSet<UWeaponHealthSet>())
	{
		const float CurrentShield = HS->GetShield();
		const float MaxShield = HS->GetMaxShield();

		if (CurrentShield < MaxShield)
		{
			if (bShieldRegenDelayed)
			{
				// 在延迟期间倒计时
				ShieldRegenTimer -= DeltaTime;
				if (ShieldRegenTimer <= 0.0f)
				{
					bShieldRegenDelayed = false;
				}
			}

			if (!bShieldRegenDelayed)
			{
				// 延迟结束，按 Rate 恢复
				const float RegenRate = HS->GetShieldRegenRate();
				const float NewShield = FMath::Min(CurrentShield + RegenRate * DeltaTime, MaxShield);
				ASC->SetNumericAttributeBase(UWeaponHealthSet::GetShieldAttribute(), NewShield);
			}
		}
	}

	// --- 能量恢复 ---
	if (const UWeaponResourceSet* RS = ASC->GetSet<UWeaponResourceSet>())
	{
		const float CurrentEnergy = RS->GetEnergy();
		const float MaxEnergy = RS->GetMaxEnergy();

		if (CurrentEnergy < MaxEnergy)
		{
			const float RegenRate = RS->GetEnergyRegenRate();
			const float NewEnergy = FMath::Min(CurrentEnergy + RegenRate * DeltaTime, MaxEnergy);
			ASC->SetNumericAttributeBase(UWeaponResourceSet::GetEnergyAttribute(), NewEnergy);
		}
	}
}
