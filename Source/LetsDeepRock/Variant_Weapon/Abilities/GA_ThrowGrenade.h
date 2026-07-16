// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GA_ThrowGrenade.generated.h"

class UWeaponGrenadeDataAsset;
class AGrenadeProjectile;
class UGameplayEffect;

/**
 *  投掷手雷 GameplayAbility
 *
 *  激活流程：CommitAbility（触发 Cooldown）→ 仅服务端 deferred spawn 手雷投射物 → EndAbility
 *  手雷自身管理引信和爆炸逻辑（AGrenadeProjectile::ExplodeOnce）。
 *
 *  投掷方向：角色 Forward 向量（俯视角中角色面向鼠标）。
 *  Spawn 位置：角色位置 + 前方偏上偏移。
 *
 *  本 Ability 与 GA_Fire 同级，但独立于武器装备系统：
 *  由 WeaponCharacter::ThrowGrenade 通过 TryActivateAbilityByClass 激活。
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

protected:

	/** 手雷数据资产（编辑器中配置，定义伤害/弹道/引信等参数） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Grenade")
	TObjectPtr<UWeaponGrenadeDataAsset> GrenadeData;

	/** 手雷 Cooldown GE 类（Has Duration + TargetTags 授予 Cooldown.Weapon.Grenade） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Grenade")
	TSubclassOf<UGameplayEffect> GrenadeCooldownEffectClass;

	/** 服务端生成手雷投射物 */
	void ServerSpawnGrenade(const FGameplayAbilityActorInfo* ActorInfo);
};
