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
 *  激活流程：计算瞄准方向 + 地面命中点 → CommitAbility（触发 Cooldown）
 *           → 仅服务端 deferred spawn 手雷投射物 → 动态弹道解算 → EndAbility
 *  手雷自身管理引信和爆炸逻辑（AGrenadeProjectile::ExplodeOnce）。
 *
 *  瞄准方向与命中点：主路径通过 GetHitResultUnderCursorByChannel(ECC_Visibility) 获取
 *   鼠标实际地面命中点（缓存为 CachedGroundHitPoint），计算从 Avatar 到命中点的 2D 归一化方向；
 *   无地面命中时 fallback 为 DeprojectMousePositionToWorld + Avatar Z 平面求交（命中点为交点）；
 *   最终不可用时回退为 Avatar->GetActorForwardVector().GetSafeNormal2D()（命中点无效）。
 *
 *  动态弹道解算：有有效地面命中点时，以固定 LaunchAngleDeg 仰角、SpawnLocation 为起点、
 *   命中点为终点，依据 v²=g·d²/(2·cos²θ·(d·tanθ−Δh)) 解算所需初速并 clamp 到 [MinThrowSpeed, MaxThrowSpeed]；
 *   水平距离超出 MaxThrowRange 的目标沿方向裁剪至该范围。
 *   GravityScale=0、分母不合法、无有效命中点等场景安全回退到 DataAsset->ThrowSpeed。
 *   SpawnRotation 以 LaunchAngleDeg 为 Pitch，不再使用硬编码上抛系数。
 *
 *  本地预测（LocalPredicted）：客户端 ActivateAbility 内计算瞄准方向并缓存；
 *   服务端 ServerSpawnGrenade 会优先重新计算方向与命中点（Listen Server），
 *   专用服务器无本地视口时安全回退到缓存值。
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

private:

	/**
	 * 计算 2D 瞄准方向（主路径：鼠标地面命中；fallback：Deproject+Z平面求交；失败返回 false）
	 * @param OutHitPoint 可选，地面命中时填充命中点世界坐标；Deproject fallback 时填充平面交点
	 */
	bool CalculateMouseAimDirection2D(const FGameplayAbilityActorInfo* ActorInfo, FVector2D& OutDirection, FVector* OutHitPoint = nullptr) const;

	/**
	 * 弹道速度解算：固定仰角，计算从 SpawnLocation 到 TargetPoint 所需初速
	 * 失败或不合法时回退到 Data->ThrowSpeed，结果始终 clamp 到 [MinThrowSpeed, MaxThrowSpeed]
	 */
	float CalculateThrowSpeed(const UWeaponGrenadeDataAsset* Data, const FVector& SpawnLocation, const FVector& TargetPoint, UWorld* World) const;

	/** 服务端生成手雷投射物（接收方向与命中点；内部优先自行重算） */
	void ServerSpawnGrenade(const FGameplayAbilityActorInfo* ActorInfo, const FVector2D& AimDirection2D, const FVector& HitPoint);

	/** 缓存投掷方向：当前 Ability 实例在脱手时计算，失败时回退到 Avatar Forward */
	FVector2D CachedThrowDirection2D;

	/** 缓存地面命中点（Valid only when 主路径或 Deproject fallback 成功；ZeroVector = 无效） */
	FVector CachedGroundHitPoint;
};
