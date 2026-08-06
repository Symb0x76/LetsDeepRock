// Copyright Epic Games, Inc. All Rights Reserved.

#include "AttributeInitHelper.h"
#include "AbilitySystemComponent.h"
#include "WeaponHealthSet.h"
#include "WeaponCombatSet.h"
#include "WeaponMovementSet.h"
#include "WeaponResistanceSet.h"
#include "WeaponResourceSet.h"
#include "../../Data/DataTableRows.h"

void FAttributeInitHelper::InitializePlayerAttributes(UAbilitySystemComponent* ASC, const FCharacterAttributeRow& Row)
{
	if (!ASC) return;

	// HealthSet
	if (UWeaponHealthSet* HS = const_cast<UWeaponHealthSet*>(ASC->GetSet<UWeaponHealthSet>()))
	{
		HS->InitMaxHealth(Row.MaxHealth);
		HS->InitHealth(Row.MaxHealth);  // 初始满血
		HS->InitMaxShield(Row.MaxShield);
		HS->InitShield(Row.MaxShield);  // 初始满盾
		HS->InitShieldRegenRate(Row.ShieldRegenRate);
		HS->InitShieldRegenDelay(Row.ShieldRegenDelay);
		HS->InitArmor(Row.Armor);
	}

	// CombatSet
	if (UWeaponCombatSet* CS = const_cast<UWeaponCombatSet*>(ASC->GetSet<UWeaponCombatSet>()))
	{
		CS->InitCritChance(Row.CritChance);
		CS->InitCritDamage(Row.CritDamage);
		CS->InitArmorPenetration(Row.ArmorPenetration);
		CS->InitDamageBonus(Row.DamageBonus);
		CS->InitWeakpointBonus(Row.WeakpointBonus);
		CS->InitAttackSpeed(Row.AttackSpeed);
		CS->InitAreaDamageBonus(Row.AreaDamageBonus);
	}

	// MovementSet
	if (UWeaponMovementSet* MS = const_cast<UWeaponMovementSet*>(ASC->GetSet<UWeaponMovementSet>()))
	{
		MS->InitMoveSpeed(Row.MoveSpeed);
		MS->InitSprintSpeed(Row.SprintSpeed);
		MS->InitDodgeDistance(Row.DodgeDistance);
	}

	// ResistanceSet
	if (UWeaponResistanceSet* RS = const_cast<UWeaponResistanceSet*>(ASC->GetSet<UWeaponResistanceSet>()))
	{
		RS->InitResistPhysical(Row.ResistPhysical);
		RS->InitResistFire(Row.ResistFire);
		RS->InitResistCold(Row.ResistCold);
		RS->InitResistPoison(Row.ResistPoison);
		RS->InitResistExplosion(Row.ResistExplosion);
		RS->InitResistElectric(Row.ResistElectric);
	}

	// ResourceSet
	if (UWeaponResourceSet* RES = const_cast<UWeaponResourceSet*>(ASC->GetSet<UWeaponResourceSet>()))
	{
		RES->InitMaxEnergy(Row.MaxEnergy);
		RES->InitEnergy(Row.MaxEnergy);  // 初始满能量
		RES->InitEnergyRegenRate(Row.EnergyRegenRate);
		RES->InitMaxStamina(Row.MaxStamina);
		RES->InitStamina(Row.MaxStamina);  // 初始满体力
		RES->InitStaminaRegenRate(Row.StaminaRegenRate);
		RES->InitLucky(Row.Lucky);
	}
}

void FAttributeInitHelper::InitializeNPCAttributes(UAbilitySystemComponent* ASC, const FEnemyConfigRow& Row)
{
	if (!ASC) return;
	if (Row.ArmorTypeTag.IsValid())
	{
		ASC->AddLooseGameplayTag(Row.ArmorTypeTag);
	}

	// HealthSet
	if (UWeaponHealthSet* HS = const_cast<UWeaponHealthSet*>(ASC->GetSet<UWeaponHealthSet>()))
	{
		HS->InitMaxHealth(Row.MaxHealth);
		HS->InitHealth(Row.MaxHealth);
		HS->InitMaxShield(Row.MaxShield);
		HS->InitShield(Row.MaxShield);
		HS->InitArmor(Row.Armor);
		// NPC 默认不设 ShieldRegen（NPC 不回盾）
	}

	// ResistanceSet
	if (UWeaponResistanceSet* RS = const_cast<UWeaponResistanceSet*>(ASC->GetSet<UWeaponResistanceSet>()))
	{
		RS->InitResistPhysical(Row.ResistPhysical);
		RS->InitResistFire(Row.ResistFire);
		RS->InitResistCold(Row.ResistCold);
		RS->InitResistPoison(Row.ResistPoison);
		RS->InitResistExplosion(Row.ResistExplosion);
		RS->InitResistElectric(Row.ResistElectric);
	}

	// MovementSet — MoveSpeed 作为倍率使用，实际速度由 CharacterMovement 的 MaxWalkSpeed 控制
	if (UWeaponMovementSet* MS = const_cast<UWeaponMovementSet*>(ASC->GetSet<UWeaponMovementSet>()))
	{
		MS->InitMoveSpeed(1.0f);
	}
}
