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

/** 投射物生成时由服务器确定、随后整体复制的初始化状态。 */
USTRUCT(BlueprintType)
struct FWeaponProjectileInitializationState
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly, Category = "Projectile")
	TSubclassOf<UGameplayEffect> DamageEffectClass;

	UPROPERTY(BlueprintReadOnly, Category = "Projectile")
	float DamageValue = 0.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Projectile")
	TObjectPtr<AActor> InstigatorActor = nullptr;

	UPROPERTY(BlueprintReadOnly, Category = "Projectile")
	FGameplayTagContainer DamageTags;

	UPROPERTY(BlueprintReadOnly, Category = "Projectile")
	float ProjectileSpeed = 3000.0f;

	UPROPERTY(BlueprintReadOnly, Category = "Projectile")
	bool bPenetratesTerrain = false;

	UPROPERTY(BlueprintReadOnly, Category = "Projectile")
	bool bInitialized = false;
};

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

	/** Deferred Spawn 完成前由服务器调用，一次写入完整初始化状态。 */
	void InitializeProjectile(TSubclassOf<UGameplayEffect> InDamageGE, float InDamage,
		AActor* InInstigator, const FGameplayTagContainer& InDamageTags,
		bool bInPenetratesTerrain, float InProjectileSpeed);

	const FWeaponProjectileInitializationState& GetInitializationState() const { return InitializationState; }
	FVector GetProjectileVelocity() const;

	/** FinishSpawning 后恢复动态运动状态；普通投射物同时恢复剩余寿命。 */
	void RestoreSnapshotMotion(const FVector& Velocity, float RemainingLifeSeconds);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

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

	/** 初始化参数作为单个复制快照发送，避免客户端观察到字段组合不一致。 */
	UPROPERTY(ReplicatedUsing = OnRep_InitializationState, BlueprintReadOnly, Category = "Projectile")
	FWeaponProjectileInitializationState InitializationState;

	/** SetByCaller 伤害 Tag */
	FGameplayTag DamageTag;

	UFUNCTION()
	void OnRep_InitializationState();

	/** 将初始化快照应用到本地移动组件和碰撞忽略列表。 */
	void ApplyInitializationState();

	/** 命中处理（碰撞回调），子类可覆写以改变命中行为（如手雷弹跳过而不引爆） */
	UFUNCTION()
	virtual void OnSphereBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
		UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
		bool bFromSweep, const FHitResult& SweepResult);

	/** 投射物停止移动时回调，子类可覆写（如手雷停止后等待引信而非立即销毁） */
	UFUNCTION()
	virtual void OnProjectileStop(const FHitResult& ImpactResult);

	/** 对目标施加伤害 GE（可用于手雷等子类的爆炸伤害） */
	virtual void ApplyDamageToTarget(AActor* TargetActor);
};
