// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AttributeSet.h"
#include "AbilitySystemComponent.h"
#include "AttributeMacros.h"
#include "UI/WeaponDebugTypes.h"
#include "WeaponHealthSet.generated.h"

class AWeaponPlayerController;

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

	//~ 护盾值
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Shield, Category = "Attributes")
	FGameplayAttributeData Shield;
	ATTRIBUTE_ACCESSORS(UWeaponHealthSet, Shield)

	//~ 最大护盾值
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_MaxShield, Category = "Attributes")
	FGameplayAttributeData MaxShield;
	ATTRIBUTE_ACCESSORS(UWeaponHealthSet, MaxShield)

	//~ 护盾每秒恢复量
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_ShieldRegenRate, Category = "Attributes")
	FGameplayAttributeData ShieldRegenRate;
	ATTRIBUTE_ACCESSORS(UWeaponHealthSet, ShieldRegenRate)

	//~ 受击后护盾恢复延迟（秒）
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_ShieldRegenDelay, Category = "Attributes")
	FGameplayAttributeData ShieldRegenDelay;
	ATTRIBUTE_ACCESSORS(UWeaponHealthSet, ShieldRegenDelay)

	//~ 护甲值（参与减伤公式）
	UPROPERTY(BlueprintReadOnly, ReplicatedUsing = OnRep_Armor, Category = "Attributes")
	FGameplayAttributeData Armor;
	ATTRIBUTE_ACCESSORS(UWeaponHealthSet, Armor)

	/** 死亡事件（服务端 Health 归零时广播），供 Owner（PlayerState/NPC）订阅执行死亡流程 */
	FOnHealthDepleted OnHealthDepleted;

	/** ExecCalc 在同步 Instant GE 扣值前暂存本次权威伤害元数据。 */
	void SetPendingWeaponDamageDebug(const FWeaponDamageDebugEvent& Event, AWeaponPlayerController* SourceController);

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

	UFUNCTION()
	void OnRep_Shield(const FGameplayAttributeData& OldShield);

	UFUNCTION()
	void OnRep_MaxShield(const FGameplayAttributeData& OldMaxShield);

	UFUNCTION()
	void OnRep_ShieldRegenRate(const FGameplayAttributeData& OldShieldRegenRate);

	UFUNCTION()
	void OnRep_ShieldRegenDelay(const FGameplayAttributeData& OldShieldRegenDelay);

	UFUNCTION()
	void OnRep_Armor(const FGameplayAttributeData& OldArmor);

private:

	/** clamp 当前 Health 到 [0, MaxHealth] 的公共入口 */
	void ClampHealth(float& NewValue) const;

	FWeaponDamageDebugEvent PendingDamageDebugEvent;
	TWeakObjectPtr<AWeaponPlayerController> PendingDamageSourceController;
	bool bHasPendingDamageDebug = false;

	/** 无论本次伤害是否实际扣值，都消费并清空 pending，避免串到后续 GE。 */
	void ConsumePendingWeaponDamageDebug(float ShieldDamage, float HealthDamage);
};
