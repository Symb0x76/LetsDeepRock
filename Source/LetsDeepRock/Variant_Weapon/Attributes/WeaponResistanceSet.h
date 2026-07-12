// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "AttributeMacros.h"
#include "WeaponResistanceSet.generated.h"

/**
 * 武器抗性属性集
 *
 * 管理物理、火焰、冰霜、毒素、爆炸、电击六种伤害类型的抗性。
 * 全部属性范围 [0, 1]，复制到客户端用于 UI 显示与伤害减免计算。
 */
UCLASS()
class UWeaponResistanceSet : public UAttributeSet
{
	GENERATED_BODY()

public:

	UWeaponResistanceSet();

	//~ 物理抗性 (0~1)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_ResistPhysical, Category = "Attributes")
	FGameplayAttributeData ResistPhysical;
	ATTRIBUTE_ACCESSORS(UWeaponResistanceSet, ResistPhysical)

	//~ 火焰抗性 (0~1)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_ResistFire, Category = "Attributes")
	FGameplayAttributeData ResistFire;
	ATTRIBUTE_ACCESSORS(UWeaponResistanceSet, ResistFire)

	//~ 冰霜抗性 (0~1)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_ResistCold, Category = "Attributes")
	FGameplayAttributeData ResistCold;
	ATTRIBUTE_ACCESSORS(UWeaponResistanceSet, ResistCold)

	//~ 毒素抗性 (0~1)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_ResistPoison, Category = "Attributes")
	FGameplayAttributeData ResistPoison;
	ATTRIBUTE_ACCESSORS(UWeaponResistanceSet, ResistPoison)

	//~ 爆炸抗性 (0~1)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_ResistExplosion, Category = "Attributes")
	FGameplayAttributeData ResistExplosion;
	ATTRIBUTE_ACCESSORS(UWeaponResistanceSet, ResistExplosion)

	//~ 电击抗性 (0~1)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_ResistElectric, Category = "Attributes")
	FGameplayAttributeData ResistElectric;
	ATTRIBUTE_ACCESSORS(UWeaponResistanceSet, ResistElectric)

protected:

	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION()
	void OnRep_ResistPhysical(const FGameplayAttributeData& OldResistPhysical);

	UFUNCTION()
	void OnRep_ResistFire(const FGameplayAttributeData& OldResistFire);

	UFUNCTION()
	void OnRep_ResistCold(const FGameplayAttributeData& OldResistCold);

	UFUNCTION()
	void OnRep_ResistPoison(const FGameplayAttributeData& OldResistPoison);

	UFUNCTION()
	void OnRep_ResistExplosion(const FGameplayAttributeData& OldResistExplosion);

	UFUNCTION()
	void OnRep_ResistElectric(const FGameplayAttributeData& OldResistElectric);
};
