// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "AttributeMacros.h"
#include "WeaponCombatSet.generated.h"

/**
 * 武器战斗属性集
 *
 * 管理暴击、破甲、伤害加成、攻击速度等战斗相关属性。
 * 全部属性复制到客户端，用于 UI 显示与伤害计算。
 */
UCLASS()
class UWeaponCombatSet : public UAttributeSet
{
	GENERATED_BODY()

public:

	UWeaponCombatSet();

	//~ 暴击率 (0~1)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_CritChance, Category = "Attributes")
	FGameplayAttributeData CritChance;
	ATTRIBUTE_ACCESSORS(UWeaponCombatSet, CritChance)

	//~ 暴击伤害倍率 (最小 1.0)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_CritDamage, Category = "Attributes")
	FGameplayAttributeData CritDamage;
	ATTRIBUTE_ACCESSORS(UWeaponCombatSet, CritDamage)

	//~ 护甲穿透率 (0~1)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_ArmorPenetration, Category = "Attributes")
	FGameplayAttributeData ArmorPenetration;
	ATTRIBUTE_ACCESSORS(UWeaponCombatSet, ArmorPenetration)

	//~ 伤害加成 (0.0 表示 +0%)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_DamageBonus, Category = "Attributes")
	FGameplayAttributeData DamageBonus;
	ATTRIBUTE_ACCESSORS(UWeaponCombatSet, DamageBonus)

	//~ 弱点额外伤害加成 (默认 0.5 表示 +50%)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_WeakpointBonus, Category = "Attributes")
	FGameplayAttributeData WeakpointBonus;
	ATTRIBUTE_ACCESSORS(UWeaponCombatSet, WeakpointBonus)

	//~ 攻击速度倍率 (最小 0.1)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_AttackSpeed, Category = "Attributes")
	FGameplayAttributeData AttackSpeed;
	ATTRIBUTE_ACCESSORS(UWeaponCombatSet, AttackSpeed)

	//~ 范围伤害加成 (0.0 表示 +0%)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_AreaDamageBonus, Category = "Attributes")
	FGameplayAttributeData AreaDamageBonus;
	ATTRIBUTE_ACCESSORS(UWeaponCombatSet, AreaDamageBonus)

protected:

	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION()
	void OnRep_CritChance(const FGameplayAttributeData& OldCritChance);

	UFUNCTION()
	void OnRep_CritDamage(const FGameplayAttributeData& OldCritDamage);

	UFUNCTION()
	void OnRep_ArmorPenetration(const FGameplayAttributeData& OldArmorPenetration);

	UFUNCTION()
	void OnRep_DamageBonus(const FGameplayAttributeData& OldDamageBonus);

	UFUNCTION()
	void OnRep_WeakpointBonus(const FGameplayAttributeData& OldWeakpointBonus);

	UFUNCTION()
	void OnRep_AttackSpeed(const FGameplayAttributeData& OldAttackSpeed);

	UFUNCTION()
	void OnRep_AreaDamageBonus(const FGameplayAttributeData& OldAreaDamageBonus);
};
