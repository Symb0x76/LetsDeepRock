// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GA_Fire.generated.h"

class UWeaponDataAsset;
class AWeaponBase;
class UGameplayEffect;

/**
 *  枪械射击 GameplayAbility
 *
 *  激活流程：CommitAbility（触发 Cooldown）→ 服务端 SpawnActor 投射物 → EndAbility
 *  投射物携带伤害 GE 类 + BaseDamage，命中时自行对目标 Apply。
 *
 *  Cooldown：通过 GetCooldownGameplayEffect() 返回 WeaponData 中配置的 CooldownGE，
 *  ApplyCooldown() 将 Spec Duration 覆盖为配置表中的 FireInterval。
 *
 *  本 Ability 通过 WeaponEquipmentComponent 授予给玩家 ASC，
 *  SourceObject 设为 WeaponEquipmentComponent，可从中获取当前武器数据。
 */
UCLASS()
class UGA_Fire : public UGameplayAbility
{
	GENERATED_BODY()

public:

	UGA_Fire();

	//~ UGameplayAbility
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility, bool bWasCancelled) override;

	/** 覆写 Cooldown GE，从 WeaponData 动态读取 CooldownEffectClass */
	virtual UGameplayEffect* GetCooldownGameplayEffect() const override;

	/** 保留 Cooldown GE 的标签配置，仅在 outgoing spec 上覆盖射击间隔 */
	virtual void ApplyCooldown(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo) const override;

protected:

	/** 获取当前装备的武器数据（通过 SourceObject → EquipmentComponent → WeaponData） */
	const UWeaponDataAsset* GetWeaponData(const FGameplayAbilityActorInfo* ActorInfo) const;

	/** 获取当前武器 Actor（用于枪口位置） */
	AWeaponBase* GetWeaponActor(const FGameplayAbilityActorInfo* ActorInfo) const;

	/** 服务端生成投射物 */
	void ServerSpawnProjectile(const FGameplayAbilityActorInfo* ActorInfo, const UWeaponDataAsset* WeaponData);
};
