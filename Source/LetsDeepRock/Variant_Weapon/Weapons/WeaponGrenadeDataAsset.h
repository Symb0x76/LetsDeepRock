// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Engine/DataAsset.h"
#include "GameplayTagContainer.h"
#include "WeaponGrenadeDataAsset.generated.h"

class AGrenadeProjectile;
class UGameplayEffect;

/**
 *  爆炸形状类型
 *
 *  控制角色伤害 Overlap 查询体和地形破坏范围的计算方式：
 *  - Sphere:  球形（Overlap）+ 圆形（地形，XY 投影距离衰减）
 *  - Capsule: 沿世界 Z 轴的胶囊体（Overlap）+ 圆形（地形，Capsule Z 轴投影 = 圆）
 *  - Box:     沿世界轴的盒形（Overlap）+ 矩形（地形，XY 投影区域内无衰减）
 *
 *  默认 Sphere，兼容既有配置（ExplosionRadius 即可）。
 */
UENUM(BlueprintType)
enum class EExplosionShape : uint8
{
	Sphere  UMETA(DisplayName = "球形"),
	Capsule UMETA(DisplayName = "胶囊体（沿Z轴）"),
	Box     UMETA(DisplayName = "盒形"),
};

/**
 *  手雷静态配置数据资产
 *
 *  独立于枪械武器系统（UWeaponDataAsset），专用于手雷投掷配置。
 *  包含投射物类、引爆参数、伤害 GE、地形破坏等所有手雷相关静态参数。
 *  编辑器中为每种手雷创建一个 DataAsset 实例（如 DA_FragGrenade）。
 */
UCLASS(BlueprintType)
class UWeaponGrenadeDataAsset : public UPrimaryDataAsset
{
	GENERATED_BODY()

public:

	/** 投射物 Actor 类（必须是 AGrenadeProjectile 子类） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Projectile")
	TSubclassOf<AGrenadeProjectile> ProjectileClass;

	/** 伤害 GE 类（Instant + SetByCaller Data.Damage） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
	TSubclassOf<UGameplayEffect> DamageEffectClass;

	/** 单发基础伤害（爆炸中心伤害值） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage", meta = (ClampMin = 0))
	float BaseDamage = 100.0f;

	/** 爆炸范围半径（cm）—— Sphere 形状的半径；Capsule 形状的胶囊半径；Box 形状的 XY 方向半尺寸参考（若 BoxExtent 未设置） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage", meta = (ClampMin = 0))
	float ExplosionRadius = 400.0f;

	/** 爆炸形状类型（角色 Overlap + 地形范围均按此计算） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
	EExplosionShape ExplosionShape = EExplosionShape::Sphere;

	/** 胶囊体半高（仅 Capsule 形状生效，cm，沿世界 Z 轴） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage", meta = (ClampMin = 0, EditCondition = "ExplosionShape == EExplosionShape::Capsule", EditConditionHides))
	float CapsuleHalfHeight = 200.0f;

	/** 盒形半尺寸（仅 Box 形状生效，cm，沿世界轴。若任一轴 ≤ 0，回退使用 ExplosionRadius 构建正方体） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage", meta = (ClampMin = 0, EditCondition = "ExplosionShape == EExplosionShape::Box", EditConditionHides))
	FVector BoxExtent = FVector(400.0f, 200.0f, 200.0f);

	// === 弹道参数 ===

	/** 引信时间（秒），从生成到引爆 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Projectile", meta = (ClampMin = 0.1))
	float FuseSeconds = 3.0f;

	/** 投掷初始速度 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Projectile", meta = (ClampMin = 100))
	float ThrowSpeed = 1500.0f;

	/** 重力缩放（1.0 = 标准重力，0 = 无重力） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Projectile", meta = (ClampMin = 0.0))
	float GravityScale = 1.0f;

	/** 是否可弹跳（撞到 WorldStatic 后反弹） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Projectile")
	bool bShouldBounce = true;

	// === 伤害 Tag ===

	/** 伤害携带的 Tag（如 Damage.Type.Explosion 用于元素抗性、Damage.Terrain 用于地形破坏） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Damage")
	FGameplayTagContainer DamageTags;

	// === 地形破坏 ===

	/** 地形最大伤害（爆炸中心值，距离衰减到 0） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Terrain", meta = (ClampMin = 0))
	float TerrainMaxDamage = 50.0f;

	/** 地形伤害半径（cm，独立于 ExplosionRadius，可设为不同值） */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Terrain", meta = (ClampMin = 0))
	float TerrainDamageRadius = 300.0f;

	// === 额外效果 ===

	/** 爆炸时对目标附加的 GE 类（如击退、眩晕等，和伤害 GE 同时生效）。空安全：null 条目自动跳过 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Effects")
	TArray<TSubclassOf<UGameplayEffect>> AdditionalTargetEffects;

	//~ UPrimaryDataAsset
	virtual FPrimaryAssetId GetPrimaryAssetId() const override
	{
		return FPrimaryAssetId(TEXT("GrenadeData"), GetFName());
	}
};
