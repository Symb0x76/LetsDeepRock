// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Abilities/GameplayAbility.h"
#include "GA_Enemy_Melee.generated.h"

class UGameplayEffect;

/**
 *  敌人专用近战 GameplayAbility
 *
 *  与玩家的 GA_Melee 不同，本 GA 不依赖 WeaponEquipmentComponent / WeaponDataAsset，
 *  所有参数（伤害、范围、角度、GE 类）直接在自身或蓝图子类中配置。
 *
 *  激活流程：CommitAbility → 服务端球形 Sweep 检测 → 对命中目标 Apply GE → EndAbility
 *
 *  使用方式：
 *  1. 在编辑器中创建本类的蓝图子类（如 BP_GA_Enemy_Melee）
 *  2. 在蓝图 Class Defaults 中配置 DamageEffectClass、MeleeDamage 等
 *  3. 在 BP_EnemyMelee 的 DefaultAbilities 数组中添加该蓝图类
 *  4. BT_EnemyCore 的 Attack Task 中选择该蓝图类作为 AbilityClass
 */
UCLASS()
class UGA_Enemy_Melee : public UGameplayAbility
{
	GENERATED_BODY()

public:

	UGA_Enemy_Melee();

	//~ UGameplayAbility
	virtual void ActivateAbility(const FGameplayAbilitySpecHandle Handle,
		const FGameplayAbilityActorInfo* ActorInfo,
		const FGameplayAbilityActivationInfo ActivationInfo,
		const FGameplayEventData* TriggerEventData) override;

protected:

	/** 伤害 GE 类（Instant + SetByCaller Data.Damage） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Combat")
	TSubclassOf<UGameplayEffect> DamageEffectClass;

	/** 近战单次伤害值 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Combat", meta = (ClampMin = 1))
	float MeleeDamage = 15.0f;

	/** 近战检测范围（球形半径，cm） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Combat", meta = (ClampMin = 10))
	float MeleeRange = 150.0f;

	/** 近战挥砍半角（度）：检测前方 ±MeleeAngle 范围内的目标 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Enemy|Combat", meta = (ClampMin = 10, ClampMax = 180))
	float MeleeAngle = 120.0f;

private:

	/** 服务端执行近战 Sweep 检测并对命中目标施加伤害 */
	void ServerMeleeSweep(const FGameplayAbilityActorInfo* ActorInfo);

	/** 对单个目标施加伤害 GE */
	void ApplyDamageToTarget(AActor* SourceActor, AActor* TargetActor);
};
