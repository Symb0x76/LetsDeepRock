// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameFramework/OnlineReplStructs.h"
#include "GameplayTagContainer.h"
#include "WeaponProjectile.generated.h"

class USphereComponent;
class UStaticMeshComponent;
class UProjectileMovementComponent;
class UGameplayEffect;

/** 投射物生成时由服务器确定、随后整体复制的内部 C++ 协议状态。 */
USTRUCT()
struct FWeaponProjectileInitializationState
{
	GENERATED_BODY()

	UPROPERTY()
	TSubclassOf<UGameplayEffect> DamageEffectClass;

	UPROPERTY()
	float DamageValue = 0.0f;

	UPROPERTY()
	TObjectPtr<AActor> InstigatorActor = nullptr;

	/** Stable source identity retained after the instigator Pawn is unpossessed. */
	UPROPERTY()
	FUniqueNetIdRepl InstigatorPlayerId;

	UPROPERTY()
	FGameplayTagContainer DamageTags;

	UPROPERTY()
	FName StatusEffectId;

	UPROPERTY()
	float ProjectileSpeed = 3000.0f;

	UPROPERTY()
	float DamageFalloffStart = 0.0f;

	UPROPERTY()
	float DamageFalloffEnd = 0.0f;

	UPROPERTY()
	float ArmorPenetration = 0.0f;

	UPROPERTY()
	float AreaDamage = 0.0f;

	UPROPERTY()
	float AreaRadius = 0.0f;

	UPROPERTY()
	float AreaFalloff = 0.0f;

	UPROPERTY()
	float ProjectileGravityScale = 0.0f;

	UPROPERTY()
	int32 MaxHitTargets = 1;

	UPROPERTY()
	bool bPenetratesTerrain = false;

	UPROPERTY()
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
		bool bInPenetratesTerrain, float InProjectileSpeed,
		float InDamageFalloffStart = 0.0f, float InDamageFalloffEnd = 0.0f,
		float InArmorPenetration = 0.0f,
		FName InStatusEffectId = NAME_None,
		const FUniqueNetIdRepl& ExplicitPlayerId = FUniqueNetIdRepl());

	const FWeaponProjectileInitializationState& GetInitializationState() const { return InitializationState; }
	FVector GetProjectileVelocity() const;

	/** FinishSpawning 后恢复动态运动状态；普通投射物同时恢复剩余寿命。 */
	void RestoreSnapshotMotion(const FVector& Velocity, float RemainingLifeSeconds);
	virtual void SetGameplayRecoveryBlocked(bool bBlocked);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 设置表驱动的生命期、重力和最大不同 Pawn 命中数 */
	void InitFlightParams(float InLifetime, float InGravityScale, int32 InMaxHitTargets);

	/** 设置范围伤害参数（由 GA_Fire 从开火模式配置传入） */
	void InitAreaDamageParams(float InAreaDamage, float InAreaRadius, float InAreaFalloff);

protected:

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

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
	UPROPERTY(ReplicatedUsing = OnRep_InitializationState)
	FWeaponProjectileInitializationState InitializationState;

	int32 MaxHitTargets = 1;
	TSet<TWeakObjectPtr<APawn>> HitPawns;
	TSet<TWeakObjectPtr<APawn>> StatusAppliedPawns;
	TOptional<FHitResult> PendingHitResult;
	float AreaDamage = 0.0f;
	float AreaRadius = 0.0f;
	float AreaFalloff = 0.0f;
	bool bHasExploded = false;

	bool bGameplayRecoveryBlocked = false;
	bool bMovementWasActiveBeforeRecovery = false;
	bool bCollisionWasEnabledBeforeRecovery = false;
	FVector RecoveryBlockedVelocity = FVector::ZeroVector;
	float RecoveryBlockedLifeSeconds = 0.0f;

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
	bool ApplyDamageToTarget(APawn* TargetPawn, TOptional<float> DamageOverride = TOptional<float>());
	virtual void ApplyDamageToTarget(AActor* TargetActor);

	/** 在指定命中点对范围内 Pawn 施加一次 GAS 伤害 */
	void Explode(const FVector& ExplosionCenter, const FHitResult* SourceHitResult = nullptr, APawn* DirectHitPawn = nullptr);
};
