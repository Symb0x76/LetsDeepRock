// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GA_Melee.generated.h"

class UWeaponDataAsset;
class UGameplayEffect;

/**
 *  近战挥砍 GameplayAbility
 *
 *  激活流程：CommitAbility（触发 Cooldown）→ 服务端球形 Sweep 检测 → 对命中目标 Apply GE_Damage → EndAbility
 *  特点：
 *  - 仅服务端执行伤害判定（SphereOverlapActors）
 *  - 同一次挥砍中 HitActors 去重（不重复扣血）
 *  - 伤害走统一链路：Source ASC → GE_Damage(SetByCaller Data.Damage) → Target ASC
 *
 *  Cooldown：通过 GetCooldownGameplayEffect() 返回 WeaponData 中配置的 MeleeCooldownEffectClass。
 *  通过 WeaponEquipmentComponent 授予给玩家 ASC。
 */
UCLASS()
class UGA_Melee : public UGameplayAbility
{
	GENERATED_BODY()

public:

	UGA_Melee();

	//~ UGameplayAbility
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

	virtual void EndAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		bool bReplicateEndAbility, bool bWasCancelled) override;

	/** 覆写 Cooldown GE，从 WeaponData 动态读取 MeleeCooldownEffectClass */
	virtual UGameplayEffect* GetCooldownGameplayEffect() const override;

protected:

	/** 获取当前装备的武器数据 */
	const UWeaponDataAsset* GetWeaponData(const FGameplayAbilityActorInfo* ActorInfo) const;

	/** 服务端执行近战 Sweep 检测并对命中目标施加伤害 */
	void ServerMeleeSweep(const FGameplayAbilityActorInfo* ActorInfo, const UWeaponDataAsset* WeaponData);

	/** 服务端执行近战对地形格子的伤害检测 */
	void ServerMeleeTerrainSweep(const FGameplayAbilityActorInfo* ActorInfo, const UWeaponDataAsset* WeaponData);

	/** 对单个目标 Apply 伤害 GE（统一伤害链路） */
	void ApplyDamageToTarget(AActor* SourceActor, AActor* TargetActor, const UWeaponDataAsset* WeaponData);
};
