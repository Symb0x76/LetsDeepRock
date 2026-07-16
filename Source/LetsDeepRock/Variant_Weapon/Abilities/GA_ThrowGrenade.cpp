// Copyright Epic Games, Inc. All Rights Reserved.

#include "GA_ThrowGrenade.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "Weapons/WeaponGrenadeDataAsset.h"
#include "Weapons/GrenadeProjectile.h"
#include "GameFramework/Character.h"
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

	// CommitAbility：检查条件 + 触发 Cooldown（无需 Cost）
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 仅服务端生成手雷投射物（bReplicates=true 自动复制到客户端）
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

	UWorld* World = Avatar->GetWorld();
	if (!World)
	{
		return;
	}

	// 投掷起始位置：角色位置 + 前方小幅偏移 + Z 轴抬高（模拟从手中抛出）
	const FVector ForwardDir = Avatar->GetActorForwardVector().GetSafeNormal2D();
	const FVector SpawnLocation = Avatar->GetActorLocation() + ForwardDir * 80.0f + FVector(0.f, 0.f, 80.0f);

	// 投掷方向：角色 Forward + 上抛仰角
	const FVector ThrowDirection = (ForwardDir + FVector(0.f, 0.f, 0.4f)).GetSafeNormal();
	const FRotator SpawnRotation = ThrowDirection.Rotation();

	const FTransform SpawnTransform(SpawnRotation, SpawnLocation);

	// 使用 Deferred Spawn：先创建 Actor（构造函数执行）但不触发 BeginPlay，
	// InitFromGrenadeData 设置好 DamageEffectClass/FuseSeconds 后
	// 再 FinishSpawning 触发 BeginPlay → Authority 引信定时器安全启动
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
		Grenade->FinishSpawning(SpawnTransform);
	}
}
