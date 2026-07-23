// Copyright Epic Games, Inc. All Rights Reserved.

#include "GA_Fire.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "Weapons/WeaponDataAsset.h"
#include "Weapons/WeaponBase.h"
#include "Weapons/WeaponProjectile.h"
#include "Weapons/WeaponEquipmentComponent.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "WeaponGameplayTags.h"
#include "WeaponCharacter.h"

UGA_Fire::UGA_Fire()
{
	// 本地预测：射击应在客户端立即响应，服务端验证
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;

	// 同一时间只能有一个 Fire 实例
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;

	// 配置 Ability Tag（用于标识和阻断）
	SetAssetTags(FGameplayTagContainer(TAG_Ability_Weapon_Fire));

	// 激活阻断 Tag：Cooldown 期间不可激活
	ActivationBlockedTags.AddTag(TAG_Cooldown_Weapon_Fire);

	// 死亡时阻断所有武器 Ability
	ActivationBlockedTags.AddTag(TAG_State_Dead);
}

void UGA_Fire::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	// 获取武器数据
	const UWeaponDataAsset* WeaponData = GetWeaponData(ActorInfo);
	if (!WeaponData)
	{
		CancelAbility(Handle, ActorInfo, ActivationInfo, true);
		return;
	}

	// CommitAbility：检查 + 触发 Cooldown（本期无 Cost）
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 仅服务端生成投射物（bReplicates=true 的投射物会自动复制到客户端）
	if (HasAuthority(&ActivationInfo))
	{
		ServerSpawnProjectile(ActorInfo, WeaponData);
	}

	// 单发射击：立即结束 Ability（Cooldown GE 已独立运行，不需要等待）
	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

void UGA_Fire::EndAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility, bool bWasCancelled)
{
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

UGameplayEffect* UGA_Fire::GetCooldownGameplayEffect() const
{
	// 从当前 ActorInfo 获取武器数据，动态返回 Cooldown GE CDO
	// GetCurrentActorInfo() 返回指针（可能为 null）
	const FGameplayAbilityActorInfo* ActorInfo = GetCurrentActorInfo();
	const UWeaponDataAsset* WeaponData = GetWeaponData(ActorInfo);
	if (WeaponData && WeaponData->CooldownEffectClass)
	{
		return WeaponData->CooldownEffectClass->GetDefaultObject<UGameplayEffect>();
	}
	// 后备：调用父类（通常返回 nullptr）
	return Super::GetCooldownGameplayEffect();
}

const UWeaponDataAsset* UGA_Fire::GetWeaponData(const FGameplayAbilityActorInfo* ActorInfo) const
{
	// 优先通过 Ability Spec 的 SourceObject 获取（GAS 标准做法，与 GA_Melee 统一）
	if (const FGameplayAbilitySpec* Spec = GetCurrentAbilitySpec())
	{
		if (const UWeaponEquipmentComponent* EquipComp = Cast<UWeaponEquipmentComponent>(Spec->SourceObject.Get()))
		{
			return EquipComp->GetCurrentWeaponData();
		}
	}

	// 后备：通过 AvatarActor 上的组件查找
	if (!ActorInfo || !ActorInfo->AvatarActor.IsValid())
	{
		return nullptr;
	}

	AActor* Avatar = ActorInfo->AvatarActor.Get();
	UWeaponEquipmentComponent* EquipComp = Avatar->FindComponentByClass<UWeaponEquipmentComponent>();
	if (!EquipComp)
	{
		return nullptr;
	}

	return EquipComp->GetCurrentWeaponData();
}

AWeaponBase* UGA_Fire::GetWeaponActor(const FGameplayAbilityActorInfo* ActorInfo) const
{
	if (!ActorInfo || !ActorInfo->AvatarActor.IsValid())
	{
		return nullptr;
	}

	AActor* Avatar = ActorInfo->AvatarActor.Get();
	UWeaponEquipmentComponent* EquipComp = Avatar->FindComponentByClass<UWeaponEquipmentComponent>();
	if (!EquipComp)
	{
		return nullptr;
	}

	return EquipComp->GetCurrentWeaponActor();
}

void UGA_Fire::ServerSpawnProjectile(const FGameplayAbilityActorInfo* ActorInfo, const UWeaponDataAsset* WeaponData)
{
	if (!WeaponData->ProjectileClass)
	{
		return;
	}

	AActor* Avatar = ActorInfo->AvatarActor.Get();
	if (!Avatar)
	{
		return;
	}

	// 确定生成位置：优先用武器 Actor 的枪口，否则用角色位置。
	FVector SpawnLocation;
	FRotator SpawnRotation;
	const FVector AimDirection = Cast<AWeaponCharacter>(Avatar)
		? Cast<AWeaponCharacter>(Avatar)->GetAimDirection().GetSafeNormal2D()
		: Avatar->GetActorForwardVector().GetSafeNormal2D();

	AWeaponBase* WeaponActor = GetWeaponActor(ActorInfo);
	if (WeaponActor)
	{
		SpawnLocation = WeaponActor->GetMuzzleLocation();
		SpawnRotation = WeaponActor->GetMuzzleRotation();
	}
	else
	{
		// 后备：从角色位置生成，方向统一使用服务器确认的 AimDirection。
		SpawnLocation = Avatar->GetActorLocation() + FVector(0.f, 0.f, 50.f);
		SpawnRotation = AimDirection.Rotation();
	}

	// 即使武器枪口自身旋转滞后，投射物仍以服务器确认的瞄准方向飞行。
	SpawnRotation = AimDirection.Rotation();

	// 俯视角射击：保持投射物水平飞行（Pitch=0）
	SpawnRotation.Pitch = 0.0f;

	// Deferred Spawn 保证初始化状态在 BeginPlay 前写入。
	const FTransform SpawnTransform(SpawnRotation, SpawnLocation);
	AWeaponProjectile* Projectile = Avatar->GetWorld()->SpawnActorDeferred<AWeaponProjectile>(
		WeaponData->ProjectileClass,
		SpawnTransform,
		Avatar,
		Cast<APawn>(Avatar),
		ESpawnActorCollisionHandlingMethod::AlwaysSpawn);

	if (Projectile)
	{
		// BeginPlay 前一次写入全部初始化参数，随后由服务器整体复制。
		Projectile->InitializeProjectile(
			WeaponData->DamageEffectClass,
			WeaponData->BaseDamage,
			Avatar,
			WeaponData->DamageTags,
			WeaponData->bProjectilePenetratesTerrain,
			WeaponData->ProjectileSpeed);
		Projectile->FinishSpawning(SpawnTransform);
	}
}
