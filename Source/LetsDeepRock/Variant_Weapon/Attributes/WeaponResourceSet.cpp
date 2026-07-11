// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponResourceSet.h"
#include "Net/UnrealNetwork.h"

UWeaponResourceSet::UWeaponResourceSet()
{
	Energy = 100.0f;
	MaxEnergy = 100.0f;
	EnergyRegenRate = 5.0f;
	Stamina = 100.0f;
	MaxStamina = 100.0f;
	StaminaRegenRate = 3.0f;
	Lucky = 0.0f;
}

void UWeaponResourceSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	if (Attribute == GetEnergyAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxEnergy());
	}
	else if (Attribute == GetMaxEnergyAttribute())
	{
		NewValue = FMath::Max(NewValue, 1.0f);
	}
	else if (Attribute == GetEnergyRegenRateAttribute())
	{
		NewValue = FMath::Max(NewValue, 0.0f);
	}
	else if (Attribute == GetStaminaAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxStamina());
	}
	else if (Attribute == GetMaxStaminaAttribute())
	{
		NewValue = FMath::Max(NewValue, 1.0f);
	}
	else if (Attribute == GetStaminaRegenRateAttribute())
	{
		NewValue = FMath::Max(NewValue, 0.0f);
	}
	else if (Attribute == GetLuckyAttribute())
	{
		NewValue = FMath::Max(NewValue, 0.0f);
	}
}

void UWeaponResourceSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponResourceSet, Energy, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponResourceSet, MaxEnergy, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponResourceSet, EnergyRegenRate, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponResourceSet, Stamina, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponResourceSet, MaxStamina, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponResourceSet, StaminaRegenRate, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponResourceSet, Lucky, COND_None, REPNOTIFY_Always);
}

void UWeaponResourceSet::OnRep_Energy(const FGameplayAttributeData& OldEnergy)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponResourceSet, Energy, OldEnergy);
}

void UWeaponResourceSet::OnRep_MaxEnergy(const FGameplayAttributeData& OldMaxEnergy)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponResourceSet, MaxEnergy, OldMaxEnergy);
}

void UWeaponResourceSet::OnRep_EnergyRegenRate(const FGameplayAttributeData& OldEnergyRegenRate)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponResourceSet, EnergyRegenRate, OldEnergyRegenRate);
}

void UWeaponResourceSet::OnRep_Stamina(const FGameplayAttributeData& OldStamina)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponResourceSet, Stamina, OldStamina);
}

void UWeaponResourceSet::OnRep_MaxStamina(const FGameplayAttributeData& OldMaxStamina)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponResourceSet, MaxStamina, OldMaxStamina);
}

void UWeaponResourceSet::OnRep_StaminaRegenRate(const FGameplayAttributeData& OldStaminaRegenRate)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponResourceSet, StaminaRegenRate, OldStaminaRegenRate);
}

void UWeaponResourceSet::OnRep_Lucky(const FGameplayAttributeData& OldLucky)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponResourceSet, Lucky, OldLucky);
}
