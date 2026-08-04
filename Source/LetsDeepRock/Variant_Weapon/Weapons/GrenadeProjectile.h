// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "WeaponProjectile.h"
#include "GameplayTagContainer.h"
#include "WeaponGrenadeDataAsset.h"
#include "GrenadeProjectile.generated.h"

class UWeaponGrenadeDataAsset;
class UGameplayEffect;

/**
 *  手雷投射物
 *
 *  继承 AWeaponProjectile，复用其伤害链路和投射物移动基础。
 *  与枪弹投射物的核心差异：
 *  - 不因碰撞 Pawn 而销毁（手雷弹跳过敌人）
 *  - 不因撞击地形而立即销毁（等待引信）
 *  - 引信到期后执行爆炸：球形范围扫描 Pawn → 按 Actor 去重 → 施加伤害
 *  - 支持对地形格子的圆形范围破坏（距离线性衰减）
 *  - 爆炸仅 Authority 执行一次（防重复爆炸）
 *  - 忽略 Owner/Instigator 碰撞
 *
 *  生命周期：Spawn → 配置引信 → 弹道飞行/弹跳 → 引信到期 ExplodeOnce → Destroy
 */
UCLASS()
class AGrenadeProjectile : public AWeaponProjectile
{
	GENERATED_BODY()

public:

	AGrenadeProjectile();

	/** 用手雷数据资产初始化（替代父类的 InitDamageParams，额外配置弹道和引信） */
	void InitFromGrenadeData(const UWeaponGrenadeDataAsset* GrenadeData, AActor* InInstigator);

	/**
	 * 设置世界空间初速度向量（替代旧的 SetInitialThrowSpeed）
	 *
	 * GA 完成弹道解算后，将完整三维初速度向量（水平 = 方向 * V * cosθ，垂直 = V * sinθ）传入。
	 * InitialSpeed = 向量长度，Velocity = 原向量，MaxSpeed 使用安全上限（不受初速度大小限制）。
	 * 避免旧实现 MaxSpeed=初速度 导致重力加速阶段下落速度被截断。
	 */
	void SetInitialThrowVelocity(const FVector& WorldVelocity);

	const FSoftObjectPath& GetGrenadeDataPath() const { return GrenadeDataPath; }
	float GetRemainingFuseTime() const;
	bool HasExploded() const { return bHasExploded; }

	/** Deferred Spawn 完成前写入快照中的剩余引信。 */
	void SetFuseTimeForSnapshotRestore(float RemainingFuseTime);

protected:

	virtual void BeginPlay() override;

	//~ 覆写碰撞回调：手雷不因 Overlap/Stop 而销毁
	virtual void OnSphereBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
		bool bFromSweep, const FHitResult& SweepResult) override;

	virtual void OnProjectileStop(const FHitResult& ImpactResult) override;

	//~ 覆写伤害施加：额外注入 DynamicAssetTags（用于 ExecCalc 元素抗性）
	virtual void ApplyDamageToTarget(AActor* TargetActor) override;

private:

	/** 引信到期回调 → ExplodeOnce */
	UFUNCTION()
	void OnFuseExpired();

	/** 执行爆炸（Authority-only，只执行一次） */
	void ExplodeOnce();

	// === 手雷配置（由 InitFromGrenadeData 设置） ===

	/** 引信时间（秒） */
	float FuseSeconds = 3.0f;

	/** 恢复时重新加载静态手雷配置所需的资产身份。 */
	UPROPERTY()
	FSoftObjectPath GrenadeDataPath;

	/** 爆炸范围（核心半径，cm。Sphere: 半径; Capsule: 胶囊半径; Box: 参考尺寸） */
	float ExplosionRadius = 400.0f;

	/** 爆炸形状类型 */
	EExplosionShape ExplosionShape = EExplosionShape::Sphere;

	/** 胶囊体半高（仅 Capsule 形状，cm，沿世界 Z 轴） */
	float CapsuleHalfHeight = 0.0f;

	/** 盒形半尺寸（仅 Box 形状，cm，沿世界轴） */
	FVector BoxExtent = FVector::ZeroVector;

	/** 地形最大伤害（中心值，距离线性衰减） */
	float TerrainMaxDamage = 0.0f;

	/** 地形伤害半径（cm） */
	float TerrainDamageRadius = 0.0f;

	/** 爆炸时对目标附加的额外 GE 类 */
	TArray<TSubclassOf<UGameplayEffect>> AdditionalTargetEffects;

	/** 防止重复爆炸 */
	bool bHasExploded = false;

	/** 引信定时器句柄 */
	FTimerHandle FuseTimerHandle;
};
