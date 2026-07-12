// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponCombatSet.h"
#include "Net/UnrealNetwork.h"

UWeaponCombatSet::UWeaponCombatSet()
{
	CritChance = 0.05f;
	CritDamage = 1.5f;
	ArmorPenetration = 0.0f;
	DamageBonus = 0.0f;
	WeakpointBonus = 0.5f;
	AttackSpeed = 1.0f;
	AreaDamageBonus = 0.0f;
}

void UWeaponCombatSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	if (Attribute == GetCritChanceAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, 1.0f);
	}
	else if (Attribute == GetArmorPenetrationAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, 1.0f);
	}
	else if (Attribute == GetAttackSpeedAttribute())
	{
		NewValue = FMath::Max(NewValue, 0.1f);
	}
	else if (Attribute == GetCritDamageAttribute())
	{
		NewValue = FMath::Max(NewValue, 1.0f);
	}
}

void UWeaponCombatSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponCombatSet, CritChance, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponCombatSet, CritDamage, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponCombatSet, ArmorPenetration, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponCombatSet, DamageBonus, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponCombatSet, WeakpointBonus, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponCombatSet, AttackSpeed, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponCombatSet, AreaDamageBonus, COND_None, REPNOTIFY_Always);
}

void UWeaponCombatSet::OnRep_CritChance(const FGameplayAttributeData& OldCritChance)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponCombatSet, CritChance, OldCritChance);
}

void UWeaponCombatSet::OnRep_CritDamage(const FGameplayAttributeData& OldCritDamage)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponCombatSet, CritDamage, OldCritDamage);
}

void UWeaponCombatSet::OnRep_ArmorPenetration(const FGameplayAttributeData& OldArmorPenetration)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponCombatSet, ArmorPenetration, OldArmorPenetration);
}

void UWeaponCombatSet::OnRep_DamageBonus(const FGameplayAttributeData& OldDamageBonus)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponCombatSet, DamageBonus, OldDamageBonus);
}

void UWeaponCombatSet::OnRep_WeakpointBonus(const FGameplayAttributeData& OldWeakpointBonus)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponCombatSet, WeakpointBonus, OldWeakpointBonus);
}

void UWeaponCombatSet::OnRep_AttackSpeed(const FGameplayAttributeData& OldAttackSpeed)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponCombatSet, AttackSpeed, OldAttackSpeed);
}

void UWeaponCombatSet::OnRep_AreaDamageBonus(const FGameplayAttributeData& OldAreaDamageBonus)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponCombatSet, AreaDamageBonus, OldAreaDamageBonus);
}
