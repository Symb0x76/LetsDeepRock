// Copyright Epic Games, Inc. All Rights Reserved.

#include "GA_ThrowGrenade.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "Weapons/WeaponGrenadeDataAsset.h"
#include "Weapons/GrenadeProjectile.h"
#include "Weapons/GrenadeBallistics.h"
#include "WeaponCharacter.h"
#include "GameFramework/Character.h"
#include "WeaponGameplayTags.h"
#include "Engine/World.h"

UGA_ThrowGrenade::UGA_ThrowGrenade()
{
	// ServerOnly：专用服务器不读取鼠标，目标由客户端 ServerRPC 发送
	// Listen Server 也走此路径（本地 Character 直接设置 pending target 后激活）
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;

	// 同一时间只能有一个 ThrowGrenade 实例
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;

	// 配置 Ability Tag
	SetAssetTags(FGameplayTagContainer(TAG_Ability_Weapon_Grenade));

	// 激活阻断 Tag：Cooldown 期间不可激活
	ActivationBlockedTags.AddTag(TAG_Cooldown_Weapon_Grenade);

	// 死亡时阻断
	ActivationBlockedTags.AddTag(TAG_State_Dead);
	// 倒地同样不能投掷
	ActivationBlockedTags.AddTag(TAG_State_Downed);
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

	// 获取 Avatar Character
	AWeaponCharacter* Character = Cast<AWeaponCharacter>(ActorInfo ? ActorInfo->AvatarActor.Get() : nullptr);
	if (!Character)
	{
		CancelAbility(Handle, ActorInfo, ActivationInfo, true);
		return;
	}

	// CommitAbility：检查条件 + 触发 Cooldown（无需 Cost）
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 仅服务端生成手雷投射物
	if (HasAuthority(&ActivationInfo))
	{
		ServerSpawnGrenade(ActorInfo);
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

void UGA_ThrowGrenade::ServerSpawnGrenade(const FGameplayAbilityActorInfo* ActorInfo)
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

	AWeaponCharacter* Character = Cast<AWeaponCharacter>(Avatar);
	if (!Character)
	{
		return;
	}

	UWorld* World = Avatar->GetWorld();
	if (!World)
	{
		return;
	}

	// ── 第1步：消费 Character 上的 PendingGrenadeTarget ──
	FVector TargetPoint = FVector::ZeroVector;
	bool bHasTarget = false;
	Character->ConsumePendingGrenadeTarget(TargetPoint, bHasTarget);

	// ── 第2步：获取瞄准方向 ──
	FVector2D AimDirection2D;
	if (bHasTarget)
	{
		const FVector ToTarget = TargetPoint - Avatar->GetActorLocation();
		AimDirection2D = FVector2D(ToTarget).GetSafeNormal();
	}
	if (AimDirection2D.SizeSquared() <= KINDA_SMALL_NUMBER)
	{
		AimDirection2D = FVector2D(Avatar->GetActorForwardVector()).GetSafeNormal();
	}

	const FVector ForwardDir(AimDirection2D.X, AimDirection2D.Y, 0.0f);

	// ── 第3步：计算 Spawn 位置 ──
	const FVector SpawnLocation = Avatar->GetActorLocation()
		+ ForwardDir * GrenadeData->SpawnForwardOffset
		+ FVector(0.0f, 0.0f, GrenadeData->SpawnUpOffset);

	// ── 第4步：使用共享弹道解算计算初速度 ──
	FGrenadeBallisticsInput BallisticsInput = FGrenadeBallisticsInput::FromDataAsset(
		GrenadeData, SpawnLocation, AimDirection2D, TargetPoint, bHasTarget, Avatar);

	FGrenadeBallisticsOutput BallisticsResult;
	FGrenadeBallistics::Calculate(BallisticsInput, World, BallisticsResult);

	// ── 第5步：Spawn 旋转 ──
	const FRotator SpawnRotation = FRotator(
		GrenadeData->LaunchAngleDeg,
		FMath::RadiansToDegrees(FMath::Atan2(AimDirection2D.Y, AimDirection2D.X)),
		0.0f);

	const FTransform SpawnTransform(SpawnRotation, SpawnLocation);

	// ── 第6步：Deferred Spawn → Init → FinishSpawning → 速度覆写 ──
	AGrenadeProjectile* Grenade = World->SpawnActorDeferred<AGrenadeProjectile>(
		GrenadeData->ProjectileClass,
		FTransform::Identity,
		Avatar,                        // Owner
		Cast<APawn>(Avatar),           // Instigator
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn
	);

	if (Grenade)
	{
		Grenade->InitFromGrenadeData(GrenadeData, Avatar);

		Grenade->FinishSpawning(SpawnTransform);

		// 使用共享解算结果的速度（包含 clamp 后值，且与预览轨迹一致）
		Grenade->SetInitialThrowVelocity(BallisticsResult.InitialVelocity);
	}
}