// Copyright Epic Games, Inc. All Rights Reserved.

#include "GA_Fire.h"
#include "AbilitySystemComponent.h"
#include "GameplayEffect.h"
#include "Weapons/WeaponDataAsset.h"
#include "Weapons/WeaponBase.h"
#include "Weapons/WeaponProjectile.h"
#include "Weapons/WeaponEquipmentComponent.h"
#include "GameFramework/Character.h"
#include "WeaponGameplayTags.h"
#include "WeaponCharacter.h"
#include "Engine/World.h"
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
	// 倒地同样不能开火
	ActivationBlockedTags.AddTag(TAG_State_Downed);
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
		if (HasAuthority(&ActivationInfo))
		{
			EquipComp->StartReload();
		}
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

	const bool bHasAuthority = HasAuthority(&ActivationInfo);
	// 一次成功射击固定消耗一发，且只由服务端权威修改弹药状态。
	if (bHasAuthority && !EquipComp->ConsumeAmmoForShot())
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
	if (bHasAuthority)
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
	const UWeaponEquipmentComponent* EquipComp = ActorInfo && ActorInfo->AvatarActor.IsValid()
		? ActorInfo->AvatarActor->FindComponentByClass<UWeaponEquipmentComponent>()
		: nullptr;
	if (WeaponData && !WeaponData->WeaponConfigId.IsNone())
	{
		if (UGameInstance* GameInstance = ActorInfo && ActorInfo->AvatarActor.IsValid()
			? ActorInfo->AvatarActor->GetGameInstance()
			: nullptr)
		{
			if (UGameDataSubsystem* DataSubsystem = GameInstance->GetSubsystem<UGameDataSubsystem>())
			{
				if (EquipComp)
				{
					if (const FWeaponFireModeConfigRow* FireModeConfig = DataSubsystem->GetWeaponFireModeConfig(EquipComp->GetCurrentFireModeId()))
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
	const UWeaponEquipmentComponent* EquipComp = ActorInfo->AvatarActor.IsValid()
		? ActorInfo->AvatarActor->FindComponentByClass<UWeaponEquipmentComponent>()
		: nullptr;
	const FName CurrentFireModeId = EquipComp ? EquipComp->GetCurrentFireModeId() : NAME_None;
	if (!WeaponData->WeaponConfigId.IsNone())
	{
		if (UGameInstance* GameInstance = ActorInfo->AvatarActor.IsValid() ? ActorInfo->AvatarActor->GetGameInstance() : nullptr)
		{
			if (UGameDataSubsystem* DataSubsystem = GameInstance->GetSubsystem<UGameDataSubsystem>())
			{
				if (EquipComp)
				{
					FireModeConfig = DataSubsystem->GetWeaponFireModeConfig(CurrentFireModeId);
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

	// 确定生成位置：优先用武器 Actor 的枪口，否则用角色位置。
	FVector SpawnLocation;
	FRotator SpawnRotation;
	const FVector AimDirection = Cast<AWeaponCharacter>(Avatar)
		? Cast<AWeaponCharacter>(Avatar)->GetAimDirection().GetSafeNormal2D()
		: Avatar->GetActorForwardVector().GetSafeNormal2D();

	if (AWeaponBase* WeaponActor = GetWeaponActor(ActorInfo))
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

	UE_LOG(LogTemp, Warning, TEXT("[GA_Fire] AimDir=(%.2f, %.2f) Yaw=%.1f SpawnYaw=%.1f CharFwd=(%.2f, %.2f)"),
		AimDirection.X, AimDirection.Y,
		AimDirection.Rotation().Yaw, SpawnRotation.Yaw,
		Avatar->GetActorForwardVector().X, Avatar->GetActorForwardVector().Y);

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
	const float DamageFalloffStart = FireModeConfig ? FireModeConfig->DamageFalloffStart : 0.0f;
	const float DamageFalloffEnd = FireModeConfig ? FireModeConfig->DamageFalloffEnd : 0.0f;
	const float ArmorPenetration = FMath::Clamp(FireModeConfig ? FireModeConfig->ArmorPenetration : 0.0f, 0.0f, 1.0f);

#if !UE_BUILD_SHIPPING
	UE_LOG(LogTemp, Log, TEXT("[FalloffTrace] GA_Fire CurrentFireModeId=%s FireModeConfigFound=%s BaseDamage=%.3f FalloffStart=%.3f FalloffEnd=%.3f"),
		*CurrentFireModeId.ToString(),
		FireModeConfig ? TEXT("true") : TEXT("false"),
		BaseDamage,
		DamageFalloffStart,
		DamageFalloffEnd);
#endif

	for (int32 ProjectileIndex = 0; ProjectileIndex < ProjectileCount; ++ProjectileIndex)
	{
		FRotator ProjectileRotation = SpawnRotation;
		if (HalfSpreadAngle > 0.0f)
		{
			ProjectileRotation.Yaw += FMath::FRandRange(-HalfSpreadAngle, HalfSpreadAngle);
		}

		const FTransform SpawnTransform(ProjectileRotation, SpawnLocation);
		AWeaponProjectile* Projectile = Avatar->GetWorld()->SpawnActorDeferred<AWeaponProjectile>(
			WeaponData->ProjectileClass,
			FTransform::Identity,
			Avatar,
			Cast<APawn>(Avatar),
			ESpawnActorCollisionHandlingMethod::AlwaysSpawn);

		if (Projectile)
		{
			Projectile->InitializeProjectile(
				WeaponData->DamageEffectClass,
				BaseDamage,
				Avatar,
				DamageTags,
				WeaponData->bProjectilePenetratesTerrain,
				ProjectileSpeed,
				DamageFalloffStart,
				DamageFalloffEnd,
				ArmorPenetration,
				FireModeConfig ? FireModeConfig->StatusEffectId : NAME_None);
			Projectile->InitAreaDamageParams(AreaDamage, AreaRadius, AreaFalloff);
			if (FireModeConfig)
			{
				Projectile->InitFlightParams(
					FireModeConfig->ProjectileLifetime,
					FireModeConfig->ProjectileGravityScale,
					FireModeConfig->MaxHitTargets);
			}
			Projectile->FinishSpawning(SpawnTransform);
		}
	}
}
