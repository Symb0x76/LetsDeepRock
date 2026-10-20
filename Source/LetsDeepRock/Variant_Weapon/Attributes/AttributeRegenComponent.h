// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayEffect.h"
#include "AttributeRegenComponent.generated.h"

class UGameplayEffect;
class UAbilitySystemComponent;

/**
 *  属性自动恢复组件（GE 生命周期控制器）
 *
 *  再生本身由 UGE_EnergyRegen / UGE_ShieldRegen（Infinite + Periodic）承担，
 *  本组件只负责 GE 的挂载/摘除编排：
 *  - BeginPlay（服务端）应用能量再生 GE 与护盾再生 GE；
 *  - NotifyDamageTaken() 摘除护盾 GE，延迟 ShieldRegenDelay 秒后重新应用；
 *  - 恢复屏障（IsGameplayRecoveryBlockedFor）激活期间摘除，释放后自动恢复。
 *
 *  挂载方式：在 BP_WeaponCharacter 蓝图中手动添加本组件（与旧版一致）。
 */
UCLASS(ClassGroup = (GAS), meta = (BlueprintSpawnableComponent))
class UAttributeRegenComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UAttributeRegenComponent();

	/** 通知受到伤害：摘除护盾再生 GE，ShieldRegenDelay 秒后重新应用 */
	void NotifyDamageTaken();

protected:
	virtual void BeginPlay() override;
	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	/** 获取 Owner 的 ASC */
	UAbilitySystemComponent* GetASC() const;

	/** 应用/摘除两类再生 GE（幂等；屏障激活时挂起为 pending） */
	void ApplyEnergyRegen();
	void ApplyShieldRegen();
	void RemoveEnergyRegen();
	void RemoveShieldRegen();

	/** 屏障同步：屏障激活期间摘除 GE，释放后补挂 pending 的 GE */
	void SyncWithRecoveryBarrier();

	/** 再生 GE 类（默认 C++ 构造的实现，可在 BP 里替换） */
	UPROPERTY(EditDefaultsOnly, Category = "GAS")
	TSubclassOf<UGameplayEffect> EnergyRegenEffectClass;

	UPROPERTY(EditDefaultsOnly, Category = "GAS")
	TSubclassOf<UGameplayEffect> ShieldRegenEffectClass;

	/** 活跃 GE 句柄 */
	FActiveGameplayEffectHandle EnergyRegenHandle;

	FActiveGameplayEffectHandle ShieldRegenHandle;

	/** 受击后的护盾再生延迟定时器 */
	FTimerHandle ShieldRegenDelayTimer;

	/** 屏障期间被摘除、待屏障释放后补挂的标记 */
	bool bEnergyRegenPending = false;

	bool bShieldRegenPending = false;
};
