// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GA_ThrowGrenade.generated.h"

class UWeaponGrenadeDataAsset;
class AGrenadeProjectile;
class UGameplayEffect;

/**
 *  投掷手雷 GameplayAbility（ServerOnly）
 *
 *  激活流程：
 *  1. 从 AWeaponCharacter 消费一次性 PendingGrenadeTarget（由客户端 ServerRPC 设置）
 *  2. 使用共享 FGrenadeBallistics 计算初速度（与客户端预览相同逻辑）
 *  3. CommitAbility（触发 Cooldown + 死亡阻断）
 *  4. 仅服务端 deferred spawn 手雷投射物 → SetInitialThrowVelocity → FinishSpawning
 *
 *  手雷自身管理引信和爆炸逻辑（AGrenadeProjectile::ExplodeOnce）。
 *
 *  本 Ability 与 GA_Fire 同级，但独立于武器装备系统：
 *  由 AWeaponCharacter::ServerThrowGrenade_Implementation 或
 *  Listen Server AWeaponCharacter::ThrowGrenadeCompleted 通过 TryActivateAbilityByClass 激活。
 *
 *  Dedicated Server 不读取鼠标：目标点由客户端通过 ServerRPC 发送。
 */
UCLASS()
class UGA_ThrowGrenade : public UGameplayAbility
{
	GENERATED_BODY()

public:

	UGA_ThrowGrenade();

	//~ UGameplayAbility
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility, bool bWasCancelled) override;

	/** 覆写 Cooldown GE：从 GrenadeCooldownEffectClass 动态返回（蓝图可覆写配置） */
	virtual UGameplayEffect* GetCooldownGameplayEffect() const override;

	/** 供角色预览读取与实际投掷一致的手雷数据 */
	UWeaponGrenadeDataAsset* GetGrenadeData() const { return GrenadeData; }

protected:

	/** 手雷数据资产（编辑器中配置，定义伤害/弹道/引信等参数） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Grenade")
	TObjectPtr<UWeaponGrenadeDataAsset> GrenadeData;

	/** 手雷 Cooldown GE 类（Has Duration + TargetTags 授予 Cooldown.Weapon.Grenade） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Grenade")
	TSubclassOf<UGameplayEffect> GrenadeCooldownEffectClass;

private:

	/** 服务器生成手雷投射物（使用共享弹道解算） */
	void ServerSpawnGrenade(const FGameplayAbilityActorInfo* ActorInfo);
};
