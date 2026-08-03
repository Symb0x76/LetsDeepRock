// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameplayTagContainer.h"
#include "WeaponProjectile.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UProjectileMovementComponent;
class UGameplayEffect;

/**
 *  武器投射物基类
 *
 *  携带伤害 GE 类 + BaseDamage 值，命中目标时在服务端构建 GE Spec 并 Apply。
 *  伤害链路：GA_Fire spawn → 投射物飞行 → 命中 → 找目标 ASC → 构建 Spec →
 *  AssignSetByCaller(Data.Damage) → ApplyToSelf → WeaponHealthSet 处理扣血。
 *
 *  设计决策：存 TSubclassOf<UGameplayEffect> + float，不存 FGameplayEffectSpecHandle，
 *  因为 SpecHandle 含 TSharedPtr 不适合跨网络复制。
 */
UCLASS(abstract)
class AWeaponProjectile : public AActor
{
	GENERATED_BODY()

public:

	AWeaponProjectile();

	/** 设置伤害参数（由 GA_Fire 在 Spawn 后立即调用） */
	void InitDamageParams(TSubclassOf<UGameplayEffect> InDamageGE, float InDamage, AActor* InInstigator, const FGameplayTagContainer& InDamageTags, bool bInPenetratesTerrain = false);

	/** 设置投射物速度（由 GA_Fire 在 Spawn 后调用，覆盖构造函数默认值） */
	void SetProjectileSpeed(float InSpeed);

	/** 设置表驱动的生命期、重力和最大不同 Pawn 命中数 */
	void InitFlightParams(float InLifetime, float InGravityScale, int32 InMaxHitTargets);

protected:

	virtual void BeginPlay() override;

	/** 碰撞体 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USphereComponent> CollisionSphere;

	/** 投射物网格体（视觉表现，无碰撞，附到 CollisionSphere） */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> ProjectileMesh;

	/** 投射物移动组件 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UProjectileMovementComponent> ProjectileMovement;

	/** 伤害 GE 类（由外部设置） */
	UPROPERTY()
	TSubclassOf<UGameplayEffect> DamageEffectClass;

	/** 伤害值（由外部设置，对应 SetByCaller Data.Damage） */
	UPROPERTY()
	float DamageValue = 0.0f;

	/** 射击发起者（用于 EffectContext 的因果链） */
	UPROPERTY()
	TWeakObjectPtr<AActor> InstigatorActor;

	/** 伤害携带的 Tags（用于地形伤害等类型判定） */
	FGameplayTagContainer DamageTags;

	/** 是否穿透地形障碍物 */
	bool bPenetratesTerrain = false;

	int32 MaxHitTargets = 1;
	TSet<TWeakObjectPtr<APawn>> HitPawns;
	TOptional<FHitResult> PendingHitResult;

	/** SetByCaller 伤害 Tag */
	FGameplayTag DamageTag;

	/** 命中处理（碰撞回调），子类可覆写以改变命中行为（如手雷弹跳过而不引爆） */
	UFUNCTION()
	virtual void OnSphereBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
		bool bFromSweep, const FHitResult& SweepResult);

	/** 投射物停止移动时回调，子类可覆写（如手雷停止后等待引信而非立即销毁） */
	UFUNCTION()
	virtual void OnProjectileStop(const FHitResult& ImpactResult);

	/** 对目标施加伤害 GE（可用于手雷等子类的爆炸伤害） */
	bool ApplyDamageToTarget(APawn* TargetPawn);
	virtual void ApplyDamageToTarget(AActor* TargetActor);
};
