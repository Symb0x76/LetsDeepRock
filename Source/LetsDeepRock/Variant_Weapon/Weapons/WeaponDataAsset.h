// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "WeaponDataAsset.generated.h"

class UGameplayAbility;
class UGameplayEffect;
class AWeaponProjectile;

/**
 *  武器静态配置数据资产
 *
 *  定义一种武器的所有静态参数：射击 Ability、投射物类、伤害值、射速等。
 *  运行时状态（弹药、冷却）不在这里，由 Ability/GE 管理。
 *  编辑器中为每种武器创建一个 DataAsset 实例（如 DA_Rifle、DA_Shotgun）。
 */
UCLASS(BlueprintType)
class UWeaponDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:

	/** 武器显示名称 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Info")
	FText WeaponName;

	/** === 射击参数 === */

	/** 射击 Ability 类（GA_Fire 或其子类） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat")
	TSubclassOf<UGameplayAbility> FireAbilityClass;

	/** 投射物 Actor 类（必须是 AWeaponProjectile 子类） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat")
	TSubclassOf<AWeaponProjectile> ProjectileClass;

	/** 单发基础伤害 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat", meta = (ClampMin = 0))
	float BaseDamage = 10.0f;

	/** 射击间隔（秒），用于 Cooldown GE 的 Duration。TODO: 当前未在代码中动态使用，Cooldown 由 GE 蓝图配置 Duration 决定 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat", meta = (ClampMin = 0.01))
	float FireInterval = 0.2f;

	/** 投射物初始速度 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat", meta = (ClampMin = 100))
	float ProjectileSpeed = 3000.0f;

	/** === 伤害 GE === */

	/** 伤害 GE 类（Instant + SetByCaller Data.Damage） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat")
	TSubclassOf<UGameplayEffect> DamageEffectClass;

	/** 伤害携带的 Tag（如 Damage.Terrain 可破坏地形） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat")
	FGameplayTagContainer DamageTags;

	/** Cooldown GE 类（Has Duration + TargetTags 授予 Cooldown.Weapon.Fire） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Combat")
	TSubclassOf<UGameplayEffect> CooldownEffectClass;

	/** === 近战参数 === */

	/** 近战 Ability 类（GA_Melee 或其子类） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee")
	TSubclassOf<UGameplayAbility> MeleeAbilityClass;

	/** 近战单次挥砍伤害 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee", meta = (ClampMin = 0))
	float MeleeDamage = 25.0f;

	/** 近战检测范围（球形半径，cm） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee", meta = (ClampMin = 10))
	float MeleeRange = 150.0f;

	/** 近战挥砍半角（度）：检测前方 ±MeleeAngle 范围内的目标 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee", meta = (ClampMin = 10, ClampMax = 180))
	float MeleeAngle = 90.0f;

	/** 近战 Cooldown GE 类（Has Duration + TargetTags 授予 Cooldown.Weapon.Melee） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Melee")
	TSubclassOf<UGameplayEffect> MeleeCooldownEffectClass;

	/** === 视觉 === */

	/** 武器静态网格（挂在角色手上的视觉表现） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Visual")
	TSoftObjectPtr<UStaticMesh> WeaponMesh;

	/** 枪口 Socket 名称（在武器网格上）。TODO: 当前 WeaponBase 使用固定 SceneComponent 作为枪口点，未读取此字段 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Visual")
	FName MuzzleSocketName = TEXT("Muzzle");

	//~ UPrimaryDataAsset
	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId(TEXT("WeaponData"), GetFName());
	}
};
