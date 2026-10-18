// Copyright Epic Games, Inc. All Rights Reserved.

#include "AttributeRegenComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "RegenGameplayEffects.h"
#include "TimerManager.h"
#include "WeaponGameState.h"
#include "WeaponHealthSet.h"

UAttributeRegenComponent::UAttributeRegenComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	// 属性写入已交给 Periodic GE；Tick 只做屏障状态同步，低频即可。
	PrimaryComponentTick.TickInterval = 0.5f;

	EnergyRegenEffectClass = UGE_EnergyRegen::StaticClass();
	ShieldRegenEffectClass = UGE_ShieldRegen::StaticClass();
}

void UAttributeRegenComponent::BeginPlay()
{
	Super::BeginPlay();

	if (GetOwner()->HasAuthority())
	{
		ApplyEnergyRegen();
		ApplyShieldRegen();
	}
}

void UAttributeRegenComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	GetOwner()->GetWorldTimerManager().ClearTimer(ShieldRegenDelayTimer);
	RemoveEnergyRegen();
	RemoveShieldRegen();
	Super::EndPlay(EndPlayReason);
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
	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC || !GetOwner()->HasAuthority())
	{
		return;
	}

	// 摘除护盾再生，延迟后重新应用；延迟时长读属性（支持天赋/GE 修改）。
	RemoveShieldRegen();

	const UWeaponHealthSet* HS = ASC->GetSet<UWeaponHealthSet>();
	const float Delay = HS ? FMath::Max(HS->GetShieldRegenDelay(), 0.0f) : 0.0f;
	if (Delay <= 0.0f)
	{
		ApplyShieldRegen();
		return;
	}
	GetOwner()->GetWorldTimerManager().SetTimer(
		ShieldRegenDelayTimer,
		FTimerDelegate::CreateUObject(this, &UAttributeRegenComponent::ApplyShieldRegen),
		Delay,
		false);
}

void UAttributeRegenComponent::TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);
	SyncWithRecoveryBarrier();
}

void UAttributeRegenComponent::ApplyEnergyRegen()
{
	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC || !GetOwner()->HasAuthority() || !EnergyRegenEffectClass)
	{
		return;
	}
	if (AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		bEnergyRegenPending = true;
		return;
	}
	if (!EnergyRegenHandle.IsValid())
	{
		FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
		EnergyRegenHandle = ASC->ApplyGameplayEffectToSelf(
			EnergyRegenEffectClass->GetDefaultObject<UGameplayEffect>(), 1.0f, Context);
	}
}

void UAttributeRegenComponent::ApplyShieldRegen()
{
	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC || !GetOwner()->HasAuthority() || !ShieldRegenEffectClass)
	{
		return;
	}
	if (AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		bShieldRegenPending = true;
		return;
	}
	if (!ShieldRegenHandle.IsValid())
	{
		FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
		ShieldRegenHandle = ASC->ApplyGameplayEffectToSelf(
			ShieldRegenEffectClass->GetDefaultObject<UGameplayEffect>(), 1.0f, Context);
	}
}

void UAttributeRegenComponent::RemoveEnergyRegen()
{
	bEnergyRegenPending = false;
	if (UAbilitySystemComponent* ASC = GetASC(); ASC && EnergyRegenHandle.IsValid())
	{
		ASC->RemoveActiveGameplayEffect(EnergyRegenHandle);
		EnergyRegenHandle.Invalidate();
	}
}

void UAttributeRegenComponent::RemoveShieldRegen()
{
	bShieldRegenPending = false;
	if (UAbilitySystemComponent* ASC = GetASC(); ASC && ShieldRegenHandle.IsValid())
	{
		ASC->RemoveActiveGameplayEffect(ShieldRegenHandle);
		ShieldRegenHandle.Invalidate();
	}
}

void UAttributeRegenComponent::SyncWithRecoveryBarrier()
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	const bool bBlocked = AWeaponGameState::IsGameplayRecoveryBlockedFor(this);
	if (bBlocked)
	{
		// 屏障期间不允许任何再生写入；保留 pending 标记供释放后补挂。
		if (EnergyRegenHandle.IsValid() || ShieldRegenHandle.IsValid())
		{
			if (EnergyRegenHandle.IsValid())
			{
				bEnergyRegenPending = true;
				UAbilitySystemComponent* ASC = GetASC();
				if (ASC)
				{
					ASC->RemoveActiveGameplayEffect(EnergyRegenHandle);
					EnergyRegenHandle.Invalidate();
				}
			}
			if (ShieldRegenHandle.IsValid())
			{
				bShieldRegenPending = true;
				UAbilitySystemComponent* ASC = GetASC();
				if (ASC)
				{
					ASC->RemoveActiveGameplayEffect(ShieldRegenHandle);
					ShieldRegenHandle.Invalidate();
				}
			}
		}
	}
	else
	{
		if (bEnergyRegenPending)
		{
			ApplyEnergyRegen();
		}
		if (bShieldRegenPending)
		{
			ApplyShieldRegen();
		}
	}
}
