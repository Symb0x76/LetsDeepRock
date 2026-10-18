// Copyright Epic Games, Inc. All Rights Reserved.

#include "RegenGameplayEffects.h"

#include "WeaponHealthSet.h"
#include "WeaponResourceSet.h"

namespace
{
	// 10Hz：与被替代的 AttributeRegenComponent::TickInterval 相同的平滑度。
	constexpr float RegenPeriod = 0.1f;

	void AddAttributeBasedPeriodicModifier(
		UGameplayEffect* Effect,
		const FGameplayAttribute& TargetAttribute,
		const FGameplayAttribute& RateAttribute)
	{
		// magnitude = Coefficient(=周期) × Target 的 Rate 属性当前值
		FAttributeBasedFloat Magnitude;
		Magnitude.Coefficient = FScalableFloat(RegenPeriod);
		Magnitude.BackingAttribute = FGameplayEffectAttributeCaptureDefinition(
			RateAttribute, EGameplayEffectAttributeCaptureSource::Target, false);

		FGameplayModifierInfo& Modifier = Effect->Modifiers.AddDefaulted_GetRef();
		Modifier.Attribute = TargetAttribute;
		Modifier.ModifierOp = EGameplayModOp::Additive;
		Modifier.ModifierMagnitude = FGameplayEffectModifierMagnitude(Magnitude);
	}
}

UGE_EnergyRegen::UGE_EnergyRegen()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	Period = FScalableFloat(RegenPeriod);
	AddAttributeBasedPeriodicModifier(this,
		UWeaponResourceSet::GetEnergyAttribute(),
		UWeaponResourceSet::GetEnergyRegenRateAttribute());
}

UGE_ShieldRegen::UGE_ShieldRegen()
{
	DurationPolicy = EGameplayEffectDurationType::Infinite;
	Period = FScalableFloat(RegenPeriod);
	AddAttributeBasedPeriodicModifier(this,
		UWeaponHealthSet::GetShieldAttribute(),
		UWeaponHealthSet::GetShieldRegenRateAttribute());
}
