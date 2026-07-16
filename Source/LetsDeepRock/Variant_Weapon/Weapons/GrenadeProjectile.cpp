// Copyright Epic Games, Inc. All Rights Reserved.

#include "GrenadeProjectile.h"
#include "WeaponGrenadeDataAsset.h"
#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "GameplayEffect.h"
#include "WeaponGameplayTags.h"
#include "GridMap/GridDamageHelper.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "TimerManager.h"

AGrenadeProjectile::AGrenadeProjectile()
{
	// 手雷物理特性：有重力、可弹跳
	// 在构造函数中设置合理默认值，InitFromGrenadeData 会覆写
	if (ProjectileMovement)
	{
		ProjectileMovement->ProjectileGravityScale = 1.0f;
		ProjectileMovement->bShouldBounce = true;
		ProjectileMovement->Bounciness = 0.6f;
		ProjectileMovement->Friction = 0.2f;
	}

	// 手雷不碰撞 Pawn（弹跳过敌人，不与敌人发生 Overlap/Block）
	if (CollisionSphere)
	{
		CollisionSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Ignore);
	}

	// 比子弹稍长的基础生命周期（但实际由引信控制，此为安全后备）
	InitialLifeSpan = 0.0f;
}

void AGrenadeProjectile::InitFromGrenadeData(const UWeaponGrenadeDataAsset* GrenadeData, AActor* InInstigator)
{
	if (!GrenadeData)
	{
		return;
	}

	// 调用父类 InitDamageParams：设置 DamageEffectClass、BaseDamage、Instigator、DamageTags
	InitDamageParams(
		GrenadeData->DamageEffectClass,
		GrenadeData->BaseDamage,
		InInstigator,
		GrenadeData->DamageTags,
		false // 手雷不穿透地形
	);

	// 手雷专属弹道配置
	FuseSeconds = GrenadeData->FuseSeconds;
	ExplosionRadius = GrenadeData->ExplosionRadius;
	ExplosionShape = GrenadeData->ExplosionShape;
	CapsuleHalfHeight = GrenadeData->CapsuleHalfHeight;
	BoxExtent = GrenadeData->BoxExtent;
	TerrainMaxDamage = GrenadeData->TerrainMaxDamage;
	TerrainDamageRadius = GrenadeData->TerrainDamageRadius;
	AdditionalTargetEffects = GrenadeData->AdditionalTargetEffects;

	// 配置投射物移动组件
	if (ProjectileMovement)
	{
		ProjectileMovement->InitialSpeed = GrenadeData->ThrowSpeed;
		ProjectileMovement->MaxSpeed = GrenadeData->ThrowSpeed;
		ProjectileMovement->ProjectileGravityScale = GrenadeData->GravityScale;
		ProjectileMovement->bShouldBounce = GrenadeData->bShouldBounce;
		ProjectileMovement->SetVelocityInLocalSpace(FVector(GrenadeData->ThrowSpeed, 0.f, 0.f));
	}
}

void AGrenadeProjectile::BeginPlay()
{
	Super::BeginPlay();

	// 引信定时器：FuseSeconds 后触发爆炸
	if (HasAuthority() && FuseSeconds > 0.0f)
	{
		GetWorldTimerManager().SetTimer(
			FuseTimerHandle,
			this,
			&AGrenadeProjectile::OnFuseExpired,
			FuseSeconds,
			false // 不循环，单次触发
		);
	}
}

void AGrenadeProjectile::OnSphereBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
	bool bFromSweep, const FHitResult& SweepResult)
{
	// 手雷不因碰撞 Pawn 而销毁 —— 弹跳过敌人，等待引信爆炸
	// 不做任何处理（默认 Pawn 已被设为 Ignore，此函数仅作防御性占位）
}

void AGrenadeProjectile::OnProjectileStop(const FHitResult& ImpactResult)
{
	// 手雷撞到地形后不立即销毁 —— 停在地面/反弹，等待引信爆炸
	// 不做任何处理
}

void AGrenadeProjectile::OnFuseExpired()
{
	ExplodeOnce();
}

void AGrenadeProjectile::ApplyDamageToTarget(AActor* TargetActor)
{
	// 覆写父类：额外注入 DynamicAssetTags，使 ExecCalc 能识别伤害类型（如 Damage.Type.Explosion）
	// 从而正确应用对应的元素抗性（Resist.Explosion）
	if (!DamageEffectClass || DamageValue <= 0.0f)
	{
		return;
	}

	IAbilitySystemInterface* TargetASCInterface = Cast<IAbilitySystemInterface>(TargetActor);
	if (!TargetASCInterface)
	{
		return;
	}

	UAbilitySystemComponent* TargetASC = TargetASCInterface->GetAbilitySystemComponent();
	if (!TargetASC)
	{
		return;
	}

	UAbilitySystemComponent* SourceASC = nullptr;
	if (InstigatorActor.IsValid())
	{
		if (IAbilitySystemInterface* SourceASCInterface = Cast<IAbilitySystemInterface>(InstigatorActor.Get()))
		{
			SourceASC = SourceASCInterface->GetAbilitySystemComponent();
		}
	}

	// ★ 修复：SourceASC 缺失时禁止退回到 TargetASC 构造 Spec。
	// 退回到 TargetASC 会导致"受伤者对自己造成伤害"的错误归因，
	// 使 ModMagnitudeCalculation/ExecCalc 取 Source 时拿到 Target 自身，
	// 从而影响抗性减免、击杀统计和反伤逻辑。
	if (!SourceASC)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GrenadeProjectile] ApplyDamageToTarget: SourceASC 缺失，跳过对 %s 的 GAS 伤害（InstigatorActor=%s）"),
			*GetNameSafe(TargetActor), *GetNameSafe(InstigatorActor.Get()));
		return;
	}

	FGameplayEffectContextHandle Context = SourceASC->MakeEffectContext();
	Context.AddInstigator(InstigatorActor.Get(), this);

	FGameplayEffectSpecHandle SpecHandle = SourceASC->MakeOutgoingSpec(DamageEffectClass, 1.0f, Context);
	if (!SpecHandle.IsValid())
	{
		return;
	}

	// 注入 SetByCaller 伤害值
	UAbilitySystemBlueprintLibrary::AssignTagSetByCallerMagnitude(SpecHandle, DamageTag, DamageValue);

	// ★ 关键：注入 DynamicAssetTags，使 ExecCalc 能从 Spec.GetDynamicAssetTags() 读取伤害类型 Tag
	// 用于 ⑦ 抗性减免：Damage.Type.Explosion → Resist.Explosion
	SpecHandle.Data->AppendDynamicAssetTags(DamageTags);

	SourceASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);
}

void AGrenadeProjectile::ExplodeOnce()
{
	// 防止重复爆炸（引信触发 + 可能的多端同步触发）
	if (bHasExploded)
	{
		return;
	}
	bHasExploded = true;

	// 仅在服务端执行爆炸逻辑
	if (!HasAuthority())
	{
		return;
	}

	const FVector ExplosionCenter = GetActorLocation();

	// ===== 1. 构建形状对应的碰撞体 =====
	FCollisionShape ExplosionCollision;
	switch (ExplosionShape)
	{
	case EExplosionShape::Sphere:
		ExplosionCollision = FCollisionShape::MakeSphere(ExplosionRadius);
		break;
	case EExplosionShape::Capsule:
		{
			const float HalfHeight = (CapsuleHalfHeight > 0.0f) ? CapsuleHalfHeight : ExplosionRadius;
			ExplosionCollision = FCollisionShape::MakeCapsule(ExplosionRadius, HalfHeight);
		}
		break;
	case EExplosionShape::Box:
		{
			// Box 半尺寸：优先使用 BoxExtent，若未配置则用 ExplosionRadius 构建正方体
			const FVector Extent = (BoxExtent.SizeSquared() > KINDA_SMALL_NUMBER)
				? BoxExtent
				: FVector(ExplosionRadius, ExplosionRadius, ExplosionRadius);
			ExplosionCollision = FCollisionShape::MakeBox(Extent);
		}
		break;
	default:
		ExplosionCollision = FCollisionShape::MakeSphere(ExplosionRadius);
		break;
	}

	// ===== 2. 对范围内 Pawn 施加伤害 =====
	{
		TArray<FOverlapResult> Overlaps;
		FCollisionQueryParams QueryParams;
		QueryParams.AddIgnoredActor(GetOwner());
		QueryParams.AddIgnoredActor(GetInstigator());
		QueryParams.AddIgnoredActor(this);

		FCollisionObjectQueryParams ObjectQueryParams;
		ObjectQueryParams.AddObjectTypesToQuery(ECC_Pawn);

		GetWorld()->OverlapMultiByObjectType(
			Overlaps,
			ExplosionCenter,
			FQuat::Identity,
			ObjectQueryParams,
			ExplosionCollision,
			QueryParams
		);

		// 去重集合
		TSet<AActor*> HitActors;

		for (const FOverlapResult& Overlap : Overlaps)
		{
			AActor* HitActor = Overlap.GetActor();
			if (!HitActor)
			{
				continue;
			}

			// 忽略 Owner、Instigator、自身
			if (HitActor == GetOwner() || HitActor == GetInstigator() || HitActor == this)
			{
				continue;
			}

			// 去重：同一 Actor 只扣一次血
			if (HitActors.Contains(HitActor))
			{
				continue;
			}

			// 检查目标是否实现了 IAbilitySystemInterface（有 ASC 才能吃伤害）
			IAbilitySystemInterface* TargetASCInterface = Cast<IAbilitySystemInterface>(HitActor);
			if (!TargetASCInterface)
			{
				continue;
			}

			HitActors.Add(HitActor);

			// 施加伤害 GE（走统一伤害链路 → DamageExecCalculation）
			ApplyDamageToTarget(HitActor);

			// 施加额外效果 GE（如击退、眩晕等）
			for (const TSubclassOf<UGameplayEffect>& EffectClass : AdditionalTargetEffects)
			{
				if (!EffectClass)
				{
					continue;
				}

				UAbilitySystemComponent* TargetASC = TargetASCInterface->GetAbilitySystemComponent();
				if (!TargetASC)
				{
					continue;
				}

				// 获取 Source ASC 用于正确归因
				UAbilitySystemComponent* SourceASC = nullptr;
				if (InstigatorActor.IsValid())
				{
					if (IAbilitySystemInterface* SourceASCInterface = Cast<IAbilitySystemInterface>(InstigatorActor.Get()))
					{
						SourceASC = SourceASCInterface->GetAbilitySystemComponent();
					}
				}

				// ★ 修复：SourceASC 缺失时跳过附加效果 GE，不退回 TargetASC
				if (!SourceASC)
				{
					UE_LOG(LogTemp, Warning, TEXT("[GrenadeProjectile] ExplodeOnce: SourceASC 缺失，跳过对 %s 的附加 GE %s"),
						*GetNameSafe(HitActor), *GetNameSafe(EffectClass));
					continue;
				}

				FGameplayEffectContextHandle EffectContext = SourceASC->MakeEffectContext();
				EffectContext.AddInstigator(InstigatorActor.Get(), this);

				FGameplayEffectSpecHandle SpecHandle = SourceASC->MakeOutgoingSpec(EffectClass, 1.0f, EffectContext);
				if (SpecHandle.IsValid())
				{
					SourceASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);
				}
			}
		}
	}

	// ===== 3. 对地形施加范围伤害（按形状走不同路径，均以 XY 投影判定） =====
	{
		if (TerrainMaxDamage > 0.0f && TerrainDamageRadius > 0.0f)
		{
			switch (ExplosionShape)
			{
			case EExplosionShape::Box:
				{
					// Box 形状：矩形地形伤害。BoxExtent 的 XY 分量定义矩形半尺寸；
					// 若未配置则用 TerrainDamageRadius 作为正方体半宽
					const float RectExtentX = (BoxExtent.SizeSquared() > KINDA_SMALL_NUMBER)
						? BoxExtent.X : TerrainDamageRadius;
					const float RectExtentY = (BoxExtent.SizeSquared() > KINDA_SMALL_NUMBER)
						? BoxExtent.Y : TerrainDamageRadius;
					FGridDamageHelper::TryApplyRectTerrainDamage(
						this,
						ExplosionCenter,
						RectExtentX,
						RectExtentY,
						TerrainMaxDamage,
						DamageTags
					);
				}
				break;
			case EExplosionShape::Sphere:
			case EExplosionShape::Capsule:
			default:
				// Sphere 和 Capsule（Z 轴）：XY 投影均为圆形，沿用距离衰减逻辑
				FGridDamageHelper::TryApplyCircularTerrainDamage(
					this,
					ExplosionCenter,
					TerrainDamageRadius,
					TerrainMaxDamage,
					DamageTags
				);
				break;
			}
		}
	}

	// ===== 4. 爆炸后销毁 =====
	Destroy();
}
