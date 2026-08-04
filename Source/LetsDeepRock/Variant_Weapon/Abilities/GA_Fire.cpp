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
#include "Data/GameDataSubsystem.h"

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
	#if !UE_BUILD_SHIPPING
	UE_LOG(LogTemp, Log, TEXT("[FireTrace] GA_Fire ActivateAbility entered Weapon=%s"), *GetNameSafe(GetWeaponData(ActorInfo)));
	#endif
	// 获取武器数据
	const UWeaponDataAsset* WeaponData = GetWeaponData(ActorInfo);
	if (!WeaponData)
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] GA_Fire rejected: no weapon"));
		#endif
		CancelAbility(Handle, ActorInfo, ActivationInfo, true);
		return;
	}

	UWeaponEquipmentComponent* EquipComp = nullptr;
	if (const FGameplayAbilitySpec* Spec = GetCurrentAbilitySpec())
	{
		EquipComp = Cast<UWeaponEquipmentComponent>(Spec->SourceObject.Get());
	}
	if (!EquipComp && ActorInfo && ActorInfo->AvatarActor.IsValid())
	{
		EquipComp = ActorInfo->AvatarActor->FindComponentByClass<UWeaponEquipmentComponent>();
	}
	if (!EquipComp)
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] GA_Fire rejected: no equipment component Weapon=%s"), *GetNameSafe(WeaponData));
		#endif
		CancelAbility(Handle, ActorInfo, ActivationInfo, true);
		return;
	}
	if (EquipComp->IsReloading())
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] GA_Fire rejected: reloading Weapon=%s Magazine=%d Reserve=%d"),
			*GetNameSafe(WeaponData), EquipComp->GetCurrentMagazineAmmo(), EquipComp->GetCurrentReserveAmmo());
		#endif
		CancelAbility(Handle, ActorInfo, ActivationInfo, true);
		return;
	}
	if (EquipComp->GetCurrentMagazineAmmo() <= 0)
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] GA_Fire rejected: empty magazine Weapon=%s Magazine=%d Reserve=%d"),
			*GetNameSafe(WeaponData), EquipComp->GetCurrentMagazineAmmo(), EquipComp->GetCurrentReserveAmmo());
		#endif
		EquipComp->StartReload();
		CancelAbility(Handle, ActorInfo, ActivationInfo, true);
		return;
	}

	// CommitAbility：检查 + 触发 Cooldown（本期无 Cost）
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] GA_Fire CommitAbility failed Weapon=%s"), *GetNameSafe(WeaponData));
		#endif
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 一次成功射击固定消耗一发；多弹丸只影响后续 Spawn 循环。
	if (!EquipComp->ConsumeAmmoForShot())
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] GA_Fire ConsumeAmmo failed Weapon=%s Magazine=%d Reserve=%d Reloading=%s"),
			*GetNameSafe(WeaponData), EquipComp->GetCurrentMagazineAmmo(), EquipComp->GetCurrentReserveAmmo(),
			EquipComp->IsReloading() ? TEXT("true") : TEXT("false"));
		#endif
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 仅服务端生成投射物（bReplicates=true 的投射物会自动复制到客户端）
	if (HasAuthority(&ActivationInfo))
	{
		ServerSpawnProjectile(ActorInfo, WeaponData);
	}
	#if !UE_BUILD_SHIPPING
	UE_LOG(LogTemp, Log, TEXT("[FireTrace] GA_Fire shot succeeded Weapon=%s Magazine=%d Reserve=%d"),
		*GetNameSafe(WeaponData), EquipComp->GetCurrentMagazineAmmo(), EquipComp->GetCurrentReserveAmmo());
	#endif

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

void UGA_Fire::ApplyCooldown(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo) const
{
	const UWeaponDataAsset* WeaponData = GetWeaponData(ActorInfo);
	float FireInterval = WeaponData ? WeaponData->FireInterval : 0.01f;
	if (WeaponData && !WeaponData->WeaponConfigId.IsNone())
	{
		if (UGameInstance* GameInstance = ActorInfo && ActorInfo->AvatarActor.IsValid()
			? ActorInfo->AvatarActor->GetGameInstance()
			: nullptr)
		{
			if (UGameDataSubsystem* DataSubsystem = GameInstance->GetSubsystem<UGameDataSubsystem>())
			{
				if (const FWeaponConfigRow* WeaponConfig = DataSubsystem->GetWeaponConfig(WeaponData->WeaponConfigId))
				{
					if (const FWeaponFireModeConfigRow* FireModeConfig = DataSubsystem->GetWeaponFireModeConfig(WeaponConfig->DefaultFireModeId))
					{
						FireInterval = FireModeConfig->FireInterval;
					}
				}
			}
		}
	}
	const float FinalFireInterval = FMath::Max(FireInterval, 0.01f);
	#if !UE_BUILD_SHIPPING
	UE_LOG(LogTemp, Log, TEXT("[FireTrace] GA_Fire ApplyCooldown WeaponConfigId=%s FireInterval=%.4f"),
		WeaponData ? *WeaponData->WeaponConfigId.ToString() : TEXT("None"), FinalFireInterval);
	#endif

	UGameplayEffect* CooldownGE = GetCooldownGameplayEffect();
	if (!CooldownGE)
	{
		return;
	}

	FGameplayEffectSpecHandle CooldownSpec = MakeOutgoingGameplayEffectSpec(
		Handle, ActorInfo, ActivationInfo, CooldownGE->GetClass(), GetAbilityLevel(Handle, ActorInfo));
	if (CooldownSpec.IsValid())
	{
		CooldownSpec.Data->SetDuration(FinalFireInterval, true);
		ApplyGameplayEffectSpecToOwner(Handle, ActorInfo, ActivationInfo, CooldownSpec);
	}
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
	const FWeaponFireModeConfigRow* FireModeConfig = nullptr;
	if (!WeaponData->WeaponConfigId.IsNone())
	{
		if (UGameInstance* GameInstance = ActorInfo->AvatarActor.IsValid() ? ActorInfo->AvatarActor->GetGameInstance() : nullptr)
		{
			if (UGameDataSubsystem* DataSubsystem = GameInstance->GetSubsystem<UGameDataSubsystem>())
			{
				if (const FWeaponConfigRow* WeaponConfig = DataSubsystem->GetWeaponConfig(WeaponData->WeaponConfigId))
				{
					FireModeConfig = DataSubsystem->GetWeaponFireModeConfig(WeaponConfig->DefaultFireModeId);
				}
			}
		}
	}

	// 表中的类路径暂不接入：现有内容使用强类型 DataAsset 引用，避免猜测蓝图生成类路径。
	if (!WeaponData->ProjectileClass)
	{
		return;
	}

	AActor* Avatar = ActorInfo->AvatarActor.Get();
	if (!Avatar)
	{
		return;
	}

	// 确定生成位置：优先用武器 Actor 的枪口，否则用角色位置+朝向
	FVector SpawnLocation;
	FRotator SpawnRotation;

	AWeaponBase* WeaponActor = GetWeaponActor(ActorInfo);
	if (WeaponActor)
	{
		SpawnLocation = WeaponActor->GetMuzzleLocation();
		SpawnRotation = WeaponActor->GetMuzzleRotation();
	}
	else
	{
		// 后备：从角色位置 + 角色自身朝向（俯视角中角色面向鼠标，不依赖 ControlRotation）
		SpawnLocation = Avatar->GetActorLocation() + FVector(0.f, 0.f, 50.f);
		SpawnRotation = Avatar->GetActorRotation();
	}

	// 俯视角射击：保持投射物水平飞行（Pitch=0）
	SpawnRotation.Pitch = 0.0f;

	// 生成投射物
	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = Avatar;
	SpawnParams.Instigator = Cast<APawn>(Avatar);
	SpawnParams.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;

	FGameplayTagContainer DamageTags = WeaponData->DamageTags;
	if (FireModeConfig && FireModeConfig->DamageTypeTag.IsValid())
	{
		const FGameplayTag DamageTypeRootTag = FGameplayTag::RequestGameplayTag(TEXT("Damage.Type"), false);
		if (DamageTypeRootTag.IsValid())
		{
			DamageTags.RemoveTags(DamageTags.Filter(FGameplayTagContainer(DamageTypeRootTag)));
		}
		DamageTags.AddTag(FireModeConfig->DamageTypeTag);
	}

	const float BaseDamage = FireModeConfig ? FireModeConfig->BaseDamage : WeaponData->BaseDamage;
	const float ProjectileSpeed = FireModeConfig ? FireModeConfig->ProjectileSpeed : WeaponData->ProjectileSpeed;
	const int32 ProjectileCount = FMath::Max(FireModeConfig ? FireModeConfig->ProjectileCount : 1, 1);
	const float HalfSpreadAngle = FMath::Max(FireModeConfig ? FireModeConfig->SpreadAngle : 0.0f, 0.0f) * 0.5f;
	const float AreaDamage = FireModeConfig ? FireModeConfig->AreaDamage : 0.0f;
	const float AreaRadius = FireModeConfig ? FireModeConfig->AreaRadius : 0.0f;
	const float AreaFalloff = FireModeConfig ? FireModeConfig->AreaFalloff : 0.0f;

	for (int32 ProjectileIndex = 0; ProjectileIndex < ProjectileCount; ++ProjectileIndex)
	{
		FRotator ProjectileRotation = SpawnRotation;
		if (HalfSpreadAngle > 0.0f)
		{
			ProjectileRotation.Yaw += FMath::FRandRange(-HalfSpreadAngle, HalfSpreadAngle);
		}

		AWeaponProjectile* Projectile = Avatar->GetWorld()->SpawnActor<AWeaponProjectile>(
			WeaponData->ProjectileClass, SpawnLocation, ProjectileRotation, SpawnParams);

		if (Projectile)
		{
			Projectile->SetActorRotation(ProjectileRotation);
			Projectile->InitDamageParams(
				WeaponData->DamageEffectClass,
				BaseDamage,
				Avatar,
				DamageTags,
				WeaponData->bProjectilePenetratesTerrain);
			Projectile->InitAreaDamageParams(AreaDamage, AreaRadius, AreaFalloff);
			Projectile->SetProjectileSpeed(ProjectileSpeed);
			if (FireModeConfig)
			{
				Projectile->InitFlightParams(
					FireModeConfig->ProjectileLifetime,
					FireModeConfig->ProjectileGravityScale,
					FireModeConfig->MaxHitTargets);
			}
		}
	}
}
