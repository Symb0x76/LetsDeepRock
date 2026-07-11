// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "AttributeMacros.h"
#include "WeaponResourceSet.generated.h"

/**
 * 武器资源属性集
 *
 * 管理能量、耐力及各自的回复速率，以及幸运值。
 * Energy / Stamina 受对应 Max 上限约束，全部属性复制到客户端。
 */
UCLASS()
class UWeaponResourceSet : public UAttributeSet
{
	GENERATED_BODY()

public:

	UWeaponResourceSet();

	//~ 当前能量
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Energy, Category = "Attributes")
	FGameplayAttributeData Energy;
	ATTRIBUTE_ACCESSORS(UWeaponResourceSet, Energy)

	//~ 最大能量 (最小 1.0)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxEnergy, Category = "Attributes")
	FGameplayAttributeData MaxEnergy;
	ATTRIBUTE_ACCESSORS(UWeaponResourceSet, MaxEnergy)

	//~ 能量回复速率 (每秒，最小 0.0)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_EnergyRegenRate, Category = "Attributes")
	FGameplayAttributeData EnergyRegenRate;
	ATTRIBUTE_ACCESSORS(UWeaponResourceSet, EnergyRegenRate)

	//~ 当前耐力
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Stamina, Category = "Attributes")
	FGameplayAttributeData Stamina;
	ATTRIBUTE_ACCESSORS(UWeaponResourceSet, Stamina)

	//~ 最大耐力 (最小 1.0)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxStamina, Category = "Attributes")
	FGameplayAttributeData MaxStamina;
	ATTRIBUTE_ACCESSORS(UWeaponResourceSet, MaxStamina)

	//~ 耐力回复速率 (每秒，最小 0.0)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_StaminaRegenRate, Category = "Attributes")
	FGameplayAttributeData StaminaRegenRate;
	ATTRIBUTE_ACCESSORS(UWeaponResourceSet, StaminaRegenRate)

	//~ 幸运值 (最小 0.0)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Lucky, Category = "Attributes")
	FGameplayAttributeData Lucky;
	ATTRIBUTE_ACCESSORS(UWeaponResourceSet, Lucky)

protected:

	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION()
	void OnRep_Energy(const FGameplayAttributeData& OldEnergy);

	UFUNCTION()
	void OnRep_MaxEnergy(const FGameplayAttributeData& OldMaxEnergy);

	UFUNCTION()
	void OnRep_EnergyRegenRate(const FGameplayAttributeData& OldEnergyRegenRate);

	UFUNCTION()
	void OnRep_Stamina(const FGameplayAttributeData& OldStamina);

	UFUNCTION()
	void OnRep_MaxStamina(const FGameplayAttributeData& OldMaxStamina);

	UFUNCTION()
	void OnRep_StaminaRegenRate(const FGameplayAttributeData& OldStaminaRegenRate);

	UFUNCTION()
	void OnRep_Lucky(const FGameplayAttributeData& OldLucky);
};
