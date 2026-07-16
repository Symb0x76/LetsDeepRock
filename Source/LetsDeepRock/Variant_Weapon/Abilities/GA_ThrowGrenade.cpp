// Copyright Epic Games, Inc. All Rights Reserved.

#include "GA_ThrowGrenade.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "Weapons/WeaponGrenadeDataAsset.h"
#include "Weapons/GrenadeProjectile.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "WeaponGameplayTags.h"

UGA_ThrowGrenade::UGA_ThrowGrenade()
{
	// 本地预测：手雷投掷客户端立即响应，服务端验证生成
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;

	// 同一时间只能有一个 ThrowGrenade 实例
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;

	// 配置 Ability Tag
	AbilityTags.AddTag(TAG_Ability_Weapon_Grenade);

	// 激活阻断 Tag：Cooldown 期间不可激活
	ActivationBlockedTags.AddTag(TAG_Cooldown_Weapon_Grenade);

	// 死亡时阻断
	ActivationBlockedTags.AddTag(TAG_State_Dead);
}

void UGA_ThrowGrenade::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	// 校验配置
	if (!GrenadeData)
	{
		CancelAbility(Handle, ActorInfo, ActivationInfo, true);
		return;
	}

	// 计算 2D 瞄准方向 + 地面命中点：主路径地面命中 → fallback Deproject+Z平面 → 最终回退 Avatar Forward
	CachedThrowDirection2D = FVector2D::ZeroVector;
	CachedGroundHitPoint = FVector::ZeroVector;
	if (!CalculateMouseAimDirection2D(ActorInfo, CachedThrowDirection2D, &CachedGroundHitPoint))
	{
		const AActor* AvatarForDir = ActorInfo ? ActorInfo->AvatarActor.Get() : nullptr;
		if (AvatarForDir)
		{
			CachedThrowDirection2D = FVector2D(AvatarForDir->GetActorForwardVector()).GetSafeNormal();
		}
		// 回退时 CachedGroundHitPoint 保持 ZeroVector，ServerSpawnGrenade 内跳过动态解算
	}

	// CommitAbility：检查条件 + 触发 Cooldown（无需 Cost）
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 仅服务端生成手雷投射物（bReplicates=true 自动复制到客户端）
	if (HasAuthority(&ActivationInfo))
	{
		ServerSpawnGrenade(ActorInfo, CachedThrowDirection2D, CachedGroundHitPoint);
	}

	// 单次投掷：立即结束 Ability（Cooldown GE 已独立运行）
	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

void UGA_ThrowGrenade::EndAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility, bool bWasCancelled)
{
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

UGameplayEffect* UGA_ThrowGrenade::GetCooldownGameplayEffect() const
{
	if (GrenadeCooldownEffectClass)
	{
		return GrenadeCooldownEffectClass->GetDefaultObject<UGameplayEffect>();
	}
	return Super::GetCooldownGameplayEffect();
}

bool UGA_ThrowGrenade::CalculateMouseAimDirection2D(const FGameplayAbilityActorInfo* ActorInfo, FVector2D& OutDirection, FVector* OutHitPoint /* = nullptr */) const
{
	if (!ActorInfo)
	{
		return false;
	}

	const AActor* Avatar = ActorInfo->AvatarActor.Get();
	if (!Avatar)
	{
		return false;
	}

	APlayerController* PC = ActorInfo->PlayerController.Get();
	if (!PC)
	{
		return false;
	}

	const FVector AvatarLocation = Avatar->GetActorLocation();

	// ── 主路径：鼠标射线地面命中（ECC_Visibility）──
	FHitResult CursorHit;
	if (PC->GetHitResultUnderCursorByChannel(UEngineTypes::ConvertToTraceType(ECC_Visibility), true, CursorHit) && CursorHit.bBlockingHit)
	{
		const FVector2D RawDir = FVector2D(CursorHit.ImpactPoint - AvatarLocation);
		OutDirection = RawDir.GetSafeNormal();
		if (OutDirection.SizeSquared() > KINDA_SMALL_NUMBER)
		{
			if (OutHitPoint)
			{
				*OutHitPoint = CursorHit.ImpactPoint;
			}
			return true;
		}
	}

	// ── Fallback：DeprojectMousePositionToWorld → Avatar Z 平面求交 ──
	FVector WorldLocation, WorldDirection;
	if (PC->DeprojectMousePositionToWorld(WorldLocation, WorldDirection))
	{
		const float TargetZ = AvatarLocation.Z;
		if (!FMath::IsNearlyZero(WorldDirection.Z))
		{
			const float T = (TargetZ - WorldLocation.Z) / WorldDirection.Z;
			if (T > 0.0f)
			{
				const FVector IntersectionPoint = WorldLocation + WorldDirection * T;
				const FVector2D RawDir = FVector2D(IntersectionPoint - AvatarLocation);
				OutDirection = RawDir.GetSafeNormal();
				if (OutDirection.SizeSquared() > KINDA_SMALL_NUMBER)
				{
					// 仅真实碰撞点可用于精确弹道解算；此路径只负责提供安全的朝向回退。
					return true;
				}
			}
		}
	}

	return false;
}

float UGA_ThrowGrenade::CalculateThrowSpeed(const UWeaponGrenadeDataAsset* Data, const FVector& SpawnLocation, const FVector& TargetPoint, UWorld* World) const
{
	if (!Data || !World)
	{
		return Data ? FMath::Clamp(Data->ThrowSpeed, Data->MinThrowSpeed, Data->MaxThrowSpeed) : 1500.0f;
	}

	const float Gravity = FMath::Abs(World->GetGravityZ()) * Data->GravityScale;

	// 无重力或重力极小：使用回退速度
	if (Gravity <= KINDA_SMALL_NUMBER)
	{
		return FMath::Clamp(Data->ThrowSpeed, Data->MinThrowSpeed, Data->MaxThrowSpeed);
	}

	const float Theta = FMath::DegreesToRadians(Data->LaunchAngleDeg);
	const float CosTheta = FMath::Cos(Theta);
	const float TanTheta = FMath::Tan(Theta);

	// 仰角接近 90°（cos 趋近 0）
	if (FMath::IsNearlyZero(CosTheta))
	{
		return FMath::Clamp(Data->ThrowSpeed, Data->MinThrowSpeed, Data->MaxThrowSpeed);
	}

	const float DeltaHeight = TargetPoint.Z - SpawnLocation.Z;   // 目标高于起抛点为正
	FVector2D HorizontalVec(TargetPoint.X - SpawnLocation.X, TargetPoint.Y - SpawnLocation.Y);
	float D = HorizontalVec.Size();

	// 距离过近
	if (D <= KINDA_SMALL_NUMBER)
	{
		return FMath::Clamp(Data->ThrowSpeed, Data->MinThrowSpeed, Data->MaxThrowSpeed);
	}

	// 超出最大射程：沿水平方向裁剪
	if (Data->MaxThrowRange > KINDA_SMALL_NUMBER && D > Data->MaxThrowRange)
	{
		const FVector2D ClampedVec = HorizontalVec.GetSafeNormal() * Data->MaxThrowRange;
		D = Data->MaxThrowRange;
		// 命中点 Z 保持原值（近似地面高度），HorizontalVec 仅用于 D 值实际已替换
	}

	const float Denominator = D * TanTheta - DeltaHeight;
	if (Denominator <= KINDA_SMALL_NUMBER)
	{
		// 分母不合法：几何上无法用此仰角命中目标（目标过高或距离过短）
		return FMath::Clamp(Data->ThrowSpeed, Data->MinThrowSpeed, Data->MaxThrowSpeed);
	}

	const float CosThetaSq = CosTheta * CosTheta;
	const float VSquared = Gravity * D * D / (2.0f * CosThetaSq * Denominator);

	if (VSquared <= 0.0f || !FMath::IsFinite(VSquared))
	{
		return FMath::Clamp(Data->ThrowSpeed, Data->MinThrowSpeed, Data->MaxThrowSpeed);
	}

	const float V = FMath::Sqrt(VSquared);
	return FMath::Clamp(V, Data->MinThrowSpeed, Data->MaxThrowSpeed);
}

void UGA_ThrowGrenade::ServerSpawnGrenade(const FGameplayAbilityActorInfo* ActorInfo, const FVector2D& AimDirection2D, const FVector& HitPoint)
{
	if (!GrenadeData || !GrenadeData->ProjectileClass)
	{
		return;
	}

	AActor* Avatar = ActorInfo->AvatarActor.Get();
	if (!Avatar)
	{
		return;
	}

	UWorld* World = Avatar->GetWorld();
	if (!World)
	{
		return;
	}

	// ── 第1步：计算瞄准方向与命中点（优先服务端重算）──
	FVector2D ThrowDir2D = AimDirection2D;
	FVector TargetHitPoint = HitPoint;
	{
		FVector2D ServerCalcDir;
		FVector ServerHitPt;
		if (CalculateMouseAimDirection2D(ActorInfo, ServerCalcDir, &ServerHitPt))
		{
			ThrowDir2D = ServerCalcDir;
			TargetHitPoint = ServerHitPt;
		}
	}

	if (ThrowDir2D.SizeSquared() <= KINDA_SMALL_NUMBER)
	{
		// 最终回退：Avatar Forward（命中点保持无效以跳过动态解算）
		ThrowDir2D = FVector2D(Avatar->GetActorForwardVector()).GetSafeNormal();
	}

	const FVector ForwardDir = FVector(ThrowDir2D.X, ThrowDir2D.Y, 0.0f);

	// ── 第2步：计算 Spawn 位置与旋转（使用 LaunchAngleDeg 作为 Pitch）──
	const FVector SpawnLocation = Avatar->GetActorLocation() + ForwardDir * 80.0f + FVector(0.f, 0.f, 80.0f);
	const FRotator SpawnRotation = FRotator(GrenadeData->LaunchAngleDeg,
		FMath::RadiansToDegrees(FMath::Atan2(ThrowDir2D.Y, ThrowDir2D.X)),
		0.0f);

	const FTransform SpawnTransform(SpawnRotation, SpawnLocation);

	// ── 第3步：Deferred Spawn → Init → 动态速度覆写 → FinishSpawning ──
	AGrenadeProjectile* Grenade = World->SpawnActorDeferred<AGrenadeProjectile>(
		GrenadeData->ProjectileClass,
		SpawnTransform,
		Avatar,                        // Owner
		Cast<APawn>(Avatar),           // Instigator
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn
	);

	if (Grenade)
	{
		Grenade->InitFromGrenadeData(GrenadeData, Avatar);

		// 动态弹道解算：仅当命中点有效时执行
		if (!TargetHitPoint.IsNearlyZero())
		{
			const float Speed = CalculateThrowSpeed(GrenadeData, SpawnLocation, TargetHitPoint, World);
			Grenade->SetInitialThrowSpeed(Speed);
		}
		// 否则 InitFromGrenadeData 已设置 DataAsset->ThrowSpeed

		Grenade->FinishSpawning(SpawnTransform);
	}
}
