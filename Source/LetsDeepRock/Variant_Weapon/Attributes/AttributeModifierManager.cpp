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

FGameplayAttribute UAttributeModifierManager::ResolveAttributeFromTag(const FGameplayTag& Tag)
{
	// 通过 Tag 映射到具体的 FGameplayAttribute（static 只初始化一次）
	static TMap<FGameplayTag, FGameplayAttribute> TagToAttribute;

	if (TagToAttribute.Num() == 0)
	{
		// --- HealthSet ---
		TagToAttribute.Add(TAG_Attr_Health, UWeaponHealthSet::GetHealthAttribute());
		TagToAttribute.Add(TAG_Attr_MaxHealth, UWeaponHealthSet::GetMaxHealthAttribute());
		TagToAttribute.Add(TAG_Attr_Shield, UWeaponHealthSet::GetShieldAttribute());
		TagToAttribute.Add(TAG_Attr_MaxShield, UWeaponHealthSet::GetMaxShieldAttribute());
		TagToAttribute.Add(TAG_Attr_ShieldRegenRate, UWeaponHealthSet::GetShieldRegenRateAttribute());
		TagToAttribute.Add(TAG_Attr_ShieldRegenDelay, UWeaponHealthSet::GetShieldRegenDelayAttribute());
		TagToAttribute.Add(TAG_Attr_Armor, UWeaponHealthSet::GetArmorAttribute());

		// --- CombatSet ---
		TagToAttribute.Add(TAG_Attr_CritChance, UWeaponCombatSet::GetCritChanceAttribute());
		TagToAttribute.Add(TAG_Attr_CritDamage, UWeaponCombatSet::GetCritDamageAttribute());
		TagToAttribute.Add(TAG_Attr_ArmorPenetration, UWeaponCombatSet::GetArmorPenetrationAttribute());
		TagToAttribute.Add(TAG_Attr_DamageBonus, UWeaponCombatSet::GetDamageBonusAttribute());
		TagToAttribute.Add(TAG_Attr_WeakpointBonus, UWeaponCombatSet::GetWeakpointBonusAttribute());
		TagToAttribute.Add(TAG_Attr_AttackSpeed, UWeaponCombatSet::GetAttackSpeedAttribute());
		TagToAttribute.Add(TAG_Attr_AreaDamageBonus, UWeaponCombatSet::GetAreaDamageBonusAttribute());

		// --- MovementSet ---
		TagToAttribute.Add(TAG_Attr_MoveSpeed, UWeaponMovementSet::GetMoveSpeedAttribute());
		TagToAttribute.Add(TAG_Attr_SprintSpeed, UWeaponMovementSet::GetSprintSpeedAttribute());
		TagToAttribute.Add(TAG_Attr_DodgeDistance, UWeaponMovementSet::GetDodgeDistanceAttribute());

		// --- ResistanceSet ---
		TagToAttribute.Add(TAG_Attr_Resist_Physical, UWeaponResistanceSet::GetResistPhysicalAttribute());
		TagToAttribute.Add(TAG_Attr_Resist_Fire, UWeaponResistanceSet::GetResistFireAttribute());
		TagToAttribute.Add(TAG_Attr_Resist_Cold, UWeaponResistanceSet::GetResistColdAttribute());
		TagToAttribute.Add(TAG_Attr_Resist_Poison, UWeaponResistanceSet::GetResistPoisonAttribute());
		TagToAttribute.Add(TAG_Attr_Resist_Explosion, UWeaponResistanceSet::GetResistExplosionAttribute());
		TagToAttribute.Add(TAG_Attr_Resist_Electric, UWeaponResistanceSet::GetResistElectricAttribute());

		// --- ResourceSet ---
		TagToAttribute.Add(TAG_Attr_Energy, UWeaponResourceSet::GetEnergyAttribute());
		TagToAttribute.Add(TAG_Attr_MaxEnergy, UWeaponResourceSet::GetMaxEnergyAttribute());
		TagToAttribute.Add(TAG_Attr_EnergyRegenRate, UWeaponResourceSet::GetEnergyRegenRateAttribute());
		TagToAttribute.Add(TAG_Attr_Stamina, UWeaponResourceSet::GetStaminaAttribute());
		TagToAttribute.Add(TAG_Attr_MaxStamina, UWeaponResourceSet::GetMaxStaminaAttribute());
		TagToAttribute.Add(TAG_Attr_StaminaRegenRate, UWeaponResourceSet::GetStaminaRegenRateAttribute());
		TagToAttribute.Add(TAG_Attr_Lucky, UWeaponResourceSet::GetLuckyAttribute());
	}

	const FGameplayAttribute* Found = TagToAttribute.Find(Tag);
	return Found ? *Found : FGameplayAttribute();
}
