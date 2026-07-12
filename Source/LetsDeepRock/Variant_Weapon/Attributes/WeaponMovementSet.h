// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "AttributeMacros.h"
#include "WeaponMovementSet.generated.h"

/**
 * 武器移动属性集
 *
 * 管理移动速度、冲刺速度、闪避距离等移动相关属性。
 * 全部属性复制到客户端，用于 UI 显示与移动计算。
 */
UCLASS()
class UWeaponMovementSet : public UAttributeSet
{
	GENERATED_BODY()

public:

	UWeaponMovementSet();

	//~ 移动速度倍率 (最小 0.0)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MoveSpeed, Category = "Attributes")
	FGameplayAttributeData MoveSpeed;
	ATTRIBUTE_ACCESSORS(UWeaponMovementSet, MoveSpeed)

	//~ 冲刺速度倍率 (最小 0.0)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_SprintSpeed, Category = "Attributes")
	FGameplayAttributeData SprintSpeed;
	ATTRIBUTE_ACCESSORS(UWeaponMovementSet, SprintSpeed)

	//~ 闪避距离 (单位 UU，最小 0.0)
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_DodgeDistance, Category = "Attributes")
	FGameplayAttributeData DodgeDistance;
	ATTRIBUTE_ACCESSORS(UWeaponMovementSet, DodgeDistance)

protected:

	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION()
	void OnRep_MoveSpeed(const FGameplayAttributeData& OldMoveSpeed);

	UFUNCTION()
	void OnRep_SprintSpeed(const FGameplayAttributeData& OldSprintSpeed);

	UFUNCTION()
	void OnRep_DodgeDistance(const FGameplayAttributeData& OldDodgeDistance);
};
