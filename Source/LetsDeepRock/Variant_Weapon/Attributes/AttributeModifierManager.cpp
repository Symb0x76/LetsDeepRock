// Copyright Epic Games, Inc. All Rights Reserved.

#include "AttributeModifierManager.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "GameplayEffect.h"
#include "WeaponHealthSet.h"
#include "WeaponCombatSet.h"
#include "WeaponMovementSet.h"
#include "WeaponResistanceSet.h"
#include "WeaponResourceSet.h"
#include "WeaponGameplayTags.h"

UAttributeModifierManager::UAttributeModifierManager()
{
	PrimaryComponentTick.bCanEverTick = false;
}

UAbilitySystemComponent* UAttributeModifierManager::GetASC() const
{
	if (IAbilitySystemInterface* ASI = Cast<IAbilitySystemInterface>(GetOwner()))
	{
		return ASI->GetAbilitySystemComponent();
	}
	return nullptr;
}

bool UAttributeModifierManager::ApplyModifiers(FName SourceId, const TArray<FAttributeModifierDef>& Modifiers)
{
	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC || Modifiers.Num() == 0)
	{
		return false;
	}

	// 如果已存在同 SourceId 的修改器，先移除
	if (ActiveModifiers.Contains(SourceId))
	{
		RemoveModifiers(SourceId);
	}

	TArray<FActiveGameplayEffectHandle>& Handles = ActiveModifiers.Add(SourceId);

	// 为每个 Modifier 创建一个独立的动态 GE
	for (const FAttributeModifierDef& ModDef : Modifiers)
	{
		FGameplayAttribute Attribute = ResolveAttributeFromTag(ModDef.AttributeTag);
		if (!Attribute.IsValid())
		{
			continue;
		}

		// 动态创建 Infinite GE
		UGameplayEffect* DynamicGE = NewObject<UGameplayEffect>(
			GetTransientPackage(),
			FName(*FString::Printf(TEXT("GE_Mod_%s_%s"), *SourceId.ToString(), *ModDef.AttributeTag.ToString()))
		);
		DynamicGE->DurationPolicy = EGameplayEffectDurationType::Infinite;

		// 添加 Modifier
		FGameplayModifierInfo& ModInfo = DynamicGE->Modifiers.AddDefaulted_GetRef();
		ModInfo.Attribute = Attribute;
		ModInfo.ModifierOp = ModDef.ModOp;

		FScalableFloat ScalableValue;
		ScalableValue.Value = ModDef.Magnitude;
		ModInfo.ModifierMagnitude = FGameplayEffectModifierMagnitude(ScalableValue);

		// Apply 到 Self
		FGameplayEffectContextHandle Context = ASC->MakeEffectContext();
		FActiveGameplayEffectHandle Handle = ASC->ApplyGameplayEffectToSelf(DynamicGE, 1.0f, Context);
		if (Handle.IsValid())
		{
			Handles.Add(Handle);
		}
	}

	return Handles.Num() > 0;
}

void UAttributeModifierManager::RemoveModifiers(FName SourceId)
{
	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC)
	{
		return;
	}

	if (TArray<FActiveGameplayEffectHandle>* Handles = ActiveModifiers.Find(SourceId))
	{
		for (const FActiveGameplayEffectHandle& Handle : *Handles)
		{
			ASC->RemoveActiveGameplayEffect(Handle);
		}
		ActiveModifiers.Remove(SourceId);
	}
}

void UAttributeModifierManager::RemoveAllModifiers()
{
	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC)
	{
		return;
	}

	for (auto& Pair : ActiveModifiers)
	{
		for (const FActiveGameplayEffectHandle& Handle : Pair.Value)
		{
			ASC->RemoveActiveGameplayEffect(Handle);
		}
	}
	ActiveModifiers.Empty();
}

bool UAttributeModifierManager::HasModifiers(FName SourceId) const
{
	return ActiveModifiers.Contains(SourceId);
}

const TMap<FGameplayTag, FGameplayAttribute>& UAttributeModifierManager::GetAttributeTagMap()
{
	static TMap<FGameplayTag, FGameplayAttribute> Map = []()
	{
		TMap<FGameplayTag, FGameplayAttribute> M;

		// --- HealthSet ---
		M.Add(TAG_Attr_Health, UWeaponHealthSet::GetHealthAttribute());
		M.Add(TAG_Attr_MaxHealth, UWeaponHealthSet::GetMaxHealthAttribute());
		M.Add(TAG_Attr_Shield, UWeaponHealthSet::GetShieldAttribute());
		M.Add(TAG_Attr_MaxShield, UWeaponHealthSet::GetMaxShieldAttribute());
		M.Add(TAG_Attr_ShieldRegenRate, UWeaponHealthSet::GetShieldRegenRateAttribute());
		M.Add(TAG_Attr_ShieldRegenDelay, UWeaponHealthSet::GetShieldRegenDelayAttribute());
		M.Add(TAG_Attr_Armor, UWeaponHealthSet::GetArmorAttribute());

		// --- CombatSet ---
		M.Add(TAG_Attr_CritChance, UWeaponCombatSet::GetCritChanceAttribute());
		M.Add(TAG_Attr_CritDamage, UWeaponCombatSet::GetCritDamageAttribute());
		M.Add(TAG_Attr_ArmorPenetration, UWeaponCombatSet::GetArmorPenetrationAttribute());
		M.Add(TAG_Attr_DamageBonus, UWeaponCombatSet::GetDamageBonusAttribute());
		M.Add(TAG_Attr_WeakpointBonus, UWeaponCombatSet::GetWeakpointBonusAttribute());
		M.Add(TAG_Attr_AttackSpeed, UWeaponCombatSet::GetAttackSpeedAttribute());
		M.Add(TAG_Attr_AreaDamageBonus, UWeaponCombatSet::GetAreaDamageBonusAttribute());

		// --- MovementSet ---
		M.Add(TAG_Attr_MoveSpeed, UWeaponMovementSet::GetMoveSpeedAttribute());
		M.Add(TAG_Attr_SprintSpeed, UWeaponMovementSet::GetSprintSpeedAttribute());
		M.Add(TAG_Attr_DodgeDistance, UWeaponMovementSet::GetDodgeDistanceAttribute());

		// --- ResistanceSet ---
		M.Add(TAG_Attr_Resist_Physical, UWeaponResistanceSet::GetResistPhysicalAttribute());
		M.Add(TAG_Attr_Resist_Fire, UWeaponResistanceSet::GetResistFireAttribute());
		M.Add(TAG_Attr_Resist_Cold, UWeaponResistanceSet::GetResistColdAttribute());
		M.Add(TAG_Attr_Resist_Poison, UWeaponResistanceSet::GetResistPoisonAttribute());
		M.Add(TAG_Attr_Resist_Explosion, UWeaponResistanceSet::GetResistExplosionAttribute());
		M.Add(TAG_Attr_Resist_Electric, UWeaponResistanceSet::GetResistElectricAttribute());

		// --- ResourceSet ---
		M.Add(TAG_Attr_Energy, UWeaponResourceSet::GetEnergyAttribute());
		M.Add(TAG_Attr_MaxEnergy, UWeaponResourceSet::GetMaxEnergyAttribute());
		M.Add(TAG_Attr_EnergyRegenRate, UWeaponResourceSet::GetEnergyRegenRateAttribute());
		M.Add(TAG_Attr_Stamina, UWeaponResourceSet::GetStaminaAttribute());
		M.Add(TAG_Attr_MaxStamina, UWeaponResourceSet::GetMaxStaminaAttribute());
		M.Add(TAG_Attr_StaminaRegenRate, UWeaponResourceSet::GetStaminaRegenRateAttribute());
		M.Add(TAG_Attr_Lucky, UWeaponResourceSet::GetLuckyAttribute());

		return M;
	}();
	return Map;
}

FGameplayAttribute UAttributeModifierManager::ResolveAttributeFromTag(const FGameplayTag& Tag)
{
	const FGameplayAttribute* Found = GetAttributeTagMap().Find(Tag);
	return Found ? *Found : FGameplayAttribute();
}
