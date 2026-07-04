// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "WeaponHealthSet.generated.h"

// 标准 GAS 属性访问器宏：为每个属性生成 Getter/Setter/Initter
#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)

/** 死亡事件委托：Health <= 0 时广播 */
DECLARE_MULTICAST_DELEGATE_OneParam(FOnHealthDepleted, AActor* /*OwningActor*/);

/**
 *  武器系统生命值属性集
 *
 *  - Health / MaxHealth：复制到客户端，用于 UI 与死亡判定
 *  - Damage：Meta Attribute（不复制），作为一次性伤害通道，
 *    在 PostGameplayEffectExecute 中转换为对 Health 的扣减
 *
 *  玩家：本属性集挂在 PlayerState 上（与 ASC 同 Owner）
 *  NPC：本属性集挂在 NPC Pawn 上（与 ASC 同 Owner）
 */
UCLASS()
class UWeaponHealthSet : public UAttributeSet
{
	GENERATED_BODY()

public:

	UWeaponHealthSet();

	//~ 当前生命值
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Health, Category = "Attributes")
	FGameplayAttributeData Health;
	ATTRIBUTE_ACCESSORS(UWeaponHealthSet, Health)

	//~ 最大生命值
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxHealth, Category = "Attributes")
	FGameplayAttributeData MaxHealth;
	ATTRIBUTE_ACCESSORS(UWeaponHealthSet, MaxHealth)

	//~ Meta 伤害通道（不复制）
	UPROPERTY(BlueprintReadOnly, Category = "Attributes")
	FGameplayAttributeData Damage;
	ATTRIBUTE_ACCESSORS(UWeaponHealthSet, Damage)

	/** 死亡事件（服务端 Health 归零时广播），供 Owner（PlayerState/NPC）订阅执行死亡流程 */
	FOnHealthDepleted OnHealthDepleted;

protected:

	//~ 属性变更前 clamp
	virtual void PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue) override;

	//~ GE 执行后处理（Meta Damage -> 扣 Health -> 判死）
	virtual void PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data) override;

	//~ 复制声明
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION()
	void OnRep_Health(const FGameplayAttributeData& OldHealth);

	UFUNCTION()
	void OnRep_MaxHealth(const FGameplayAttributeData& OldMaxHealth);

private:

	/** clamp 当前 Health 到 [0, MaxHealth] 的公共入口 */
	void ClampHealth(float& NewValue) const;
};
