// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponResistanceSet.h"
#include "Net/UnrealNetwork.h"

UWeaponResistanceSet::UWeaponResistanceSet()
{
	ResistPhysical = 0.0f;
	ResistFire = 0.0f;
	ResistCold = 0.0f;
	ResistPoison = 0.0f;
	ResistExplosion = 0.0f;
	ResistElectric = 0.0f;
}

void UWeaponResistanceSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	if (Attribute == GetResistPhysicalAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, 1.0f);
	}
	else if (Attribute == GetResistFireAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, 1.0f);
	}
	else if (Attribute == GetResistColdAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, 1.0f);
	}
	else if (Attribute == GetResistPoisonAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, 1.0f);
	}
	else if (Attribute == GetResistExplosionAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, 1.0f);
	}
	else if (Attribute == GetResistElectricAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, 1.0f);
	}
}

void UWeaponResistanceSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponResistanceSet, ResistPhysical, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponResistanceSet, ResistFire, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponResistanceSet, ResistCold, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponResistanceSet, ResistPoison, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponResistanceSet, ResistExplosion, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponResistanceSet, ResistElectric, COND_None, REPNOTIFY_Always);
}

void UWeaponResistanceSet::OnRep_ResistPhysical(const FGameplayAttributeData& OldResistPhysical)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponResistanceSet, ResistPhysical, OldResistPhysical);
}

void UWeaponResistanceSet::OnRep_ResistFire(const FGameplayAttributeData& OldResistFire)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponResistanceSet, ResistFire, OldResistFire);
}

void UWeaponResistanceSet::OnRep_ResistCold(const FGameplayAttributeData& OldResistCold)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponResistanceSet, ResistCold, OldResistCold);
}

void UWeaponResistanceSet::OnRep_ResistPoison(const FGameplayAttributeData& OldResistPoison)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponResistanceSet, ResistPoison, OldResistPoison);
}

void UWeaponResistanceSet::OnRep_ResistExplosion(const FGameplayAttributeData& OldResistExplosion)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponResistanceSet, ResistExplosion, OldResistExplosion);
}

void UWeaponResistanceSet::OnRep_ResistElectric(const FGameplayAttributeData& OldResistElectric)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponResistanceSet, ResistElectric, OldResistElectric);
}
