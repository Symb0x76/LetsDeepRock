// Copyright Epic Games, Inc. All Rights Reserved.

#include "GA_Melee.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "GameplayEffect.h"
#include "Weapons/WeaponDataAsset.h"
#include "Weapons/WeaponEquipmentComponent.h"
#include "GameFramework/Character.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "GameplayTagContainer.h"
#include "WeaponGameplayTags.h"
#include "GridMap/GridDamageHelper.h"
#include "WeaponCharacter.h"
#include "Swarm/SwarmDamageRouting.h"

UGA_Melee::UGA_Melee()
{
	// 本地预测：近战客户端立即响应（播动画），服务端验证伤害
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::LocalPredicted;

	// 同一时间只能有一个 Melee 实例
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;

	// 配置 Ability Tag
	SetAssetTags(FGameplayTagContainer(TAG_Ability_Weapon_Melee));

	// 激活阻断 Tag：Cooldown 期间不可激活
	ActivationBlockedTags.AddTag(TAG_Cooldown_Weapon_Melee);

	// 死亡时阻断所有武器 Ability
	ActivationBlockedTags.AddTag(TAG_State_Dead);
}

void UGA_Melee::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
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

	// CommitAbility：检查条件 + 触发 Cooldown
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 仅服务端执行 Sweep 伤害判定
	if (HasAuthority(&ActivationInfo))
	{
		ServerMeleeSweep(ActorInfo, WeaponData);
		ServerMeleeTerrainSweep(ActorInfo, WeaponData);
	}

	// 单次挥砍：立即结束 Ability（Cooldown GE 独立运行）
	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

void UGA_Melee::EndAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	bool bReplicateEndAbility, bool bWasCancelled)
{
	Super::EndAbility(Handle, ActorInfo, ActivationInfo, bReplicateEndAbility, bWasCancelled);
}

UGameplayEffect* UGA_Melee::GetCooldownGameplayEffect() const
{
	const FGameplayAbilityActorInfo* ActorInfo = GetCurrentActorInfo();
	const UWeaponDataAsset* WeaponData = GetWeaponData(ActorInfo);
	if (WeaponData && WeaponData->MeleeCooldownEffectClass)
	{
		return WeaponData->MeleeCooldownEffectClass->GetDefaultObject<UGameplayEffect>();
	}
	return Super::GetCooldownGameplayEffect();
}

const UWeaponDataAsset* UGA_Melee::GetWeaponData(const FGameplayAbilityActorInfo* ActorInfo) const
{
	// 优先通过 Ability Spec 的 SourceObject 获取（GAS 标准做法）
	if (const FGameplayAbilitySpec* Spec = GetCurrentAbilitySpec())
	{
		if (const UWeaponEquipmentComponent* EquipComp = Cast<UWeaponEquipmentComponent>(Spec->SourceObject.Get()))
		{
			return EquipComp->GetCurrentWeaponData();
		}
	}

	// 后备：通过 AvatarActor 上的组件查找
	if (ActorInfo && ActorInfo->AvatarActor.IsValid())
	{
		AActor* Avatar = ActorInfo->AvatarActor.Get();
		UWeaponEquipmentComponent* EquipComp = Avatar->FindComponentByClass<UWeaponEquipmentComponent>();
		if (EquipComp)
		{
			return EquipComp->GetCurrentWeaponData();
		}
	}

	return nullptr;
}

void UGA_Melee::ServerMeleeSweep(const FGameplayAbilityActorInfo* ActorInfo, const UWeaponDataAsset* WeaponData)
{
	AActor* Avatar = ActorInfo->AvatarActor.Get();
	if (!Avatar)
	{
		return;
	}

	// Sweep 中心放角色位置，半径 = MeleeRange
	const FVector AvatarLocation = Avatar->GetActorLocation();
	const AWeaponCharacter* WeaponCharacter = Cast<AWeaponCharacter>(Avatar);
	const FVector ForwardDir = WeaponCharacter
		? WeaponCharacter->GetAimDirection().GetSafeNormal2D()
		: Avatar->GetActorForwardVector().GetSafeNormal2D();
	const float SweepRadius = WeaponData->MeleeRange;

	// 球形重叠检测：按 Pawn ObjectType 查询
	TArray<FOverlapResult> Overlaps;
	FCollisionShape SweepShape = FCollisionShape::MakeSphere(SweepRadius);
	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(Avatar);  // 忽略自己

	FCollisionObjectQueryParams ObjectQueryParams;
	ObjectQueryParams.AddObjectTypesToQuery(ECC_Pawn);

	Avatar->GetWorld()->OverlapMultiByObjectType(
		Overlaps,
		AvatarLocation,
		FQuat::Identity,
		ObjectQueryParams,
		SweepShape,
		QueryParams
	);

	// 预计算角度阈值（Dot 值比较，避免每次 Acos）
	const float MinDot = FMath::Cos(FMath::DegreesToRadians(WeaponData->MeleeAngle));

	// HitActors 去重集合（同一次挥砍不重复扣血）
	TSet<AActor*> HitActors;

	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* HitActor = Overlap.GetActor();
		if (!HitActor || HitActor == Avatar)
		{
			continue;
		}

		// 去重：同一 Actor 只处理一次
		if (HitActors.Contains(HitActor))
		{
			continue;
		}

		// 距离校验：确保目标在 MeleeRange 内（2D 距离）
		const FVector ToTargetVector = HitActor->GetActorLocation() - AvatarLocation;
		const float Distance2D = ToTargetVector.Size2D();
		if (Distance2D > WeaponData->MeleeRange)
		{
			continue;
		}

		// 扇形角度过滤：用 Dot 阈值判断（比 Acos 高效）
		const FVector ToTarget = ToTargetVector.GetSafeNormal2D();
		const float DotProduct = FVector::DotProduct(ForwardDir, ToTarget);
		if (DotProduct < MinDot)
		{
			continue;
		}

		// 通过所有检查，加入 HitActors 并施加伤害
		HitActors.Add(HitActor);
		ApplyDamageToTarget(Avatar, HitActor, WeaponData);
	}
}

void UGA_Melee::ApplyDamageToTarget(AActor* SourceActor, AActor* TargetActor, const UWeaponDataAsset* WeaponData)
{
	// 仅服务端允许施加伤害
	if (!SourceActor || !SourceActor->HasAuthority())
	{
		return;
	}

	if (!TargetActor || !WeaponData || !WeaponData->DamageEffectClass)
	{
		return;
	}

	// 虫群目标：无 ASC，绕过 GAS 直接转发伤害事件
	{
		const FSwarmDamageEvent SwarmEvent{ WeaponData->MeleeDamage, FGameplayTagContainer(), NAME_None, SourceActor };
		if (SwarmTryRouteDamage(TargetActor, SwarmEvent))
		{
			return;
		}
	}

	// 获取目标 ASC
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

	// 获取 Source ASC（射击者/挥砍者）
	IAbilitySystemInterface* SourceASCInterface = Cast<IAbilitySystemInterface>(SourceActor);
	if (!SourceASCInterface)
	{
		return;
	}
	UAbilitySystemComponent* SourceASC = SourceASCInterface->GetAbilitySystemComponent();
	if (!SourceASC)
	{
		return;
	}

	// 构建 EffectContext（来源是挥砍者）
	FGameplayEffectContextHandle Context = SourceASC->MakeEffectContext();
	Context.AddSourceObject(SourceActor);

	// 创建 GE Spec
	FGameplayEffectSpecHandle SpecHandle = SourceASC->MakeOutgoingSpec(
		WeaponData->DamageEffectClass, 1.0f, Context);

	if (!SpecHandle.IsValid())
	{
		return;
	}

	// 设置 SetByCaller 伤害值（使用近战伤害）
	const FGameplayTag DamageTag = TAG_Data_Damage;
	SpecHandle.Data->SetSetByCallerMagnitude(DamageTag, WeaponData->MeleeDamage);

	// 施加伤害：Source ASC → Target ASC
	SourceASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);
}

void UGA_Melee::ServerMeleeTerrainSweep(const FGameplayAbilityActorInfo* ActorInfo, const UWeaponDataAsset* WeaponData)
{
	if (!WeaponData || !FGridDamageHelper::HasTerrainDamageTag(WeaponData->DamageTags))
	{
		return; // 该武器不携带地形伤害 Tag，跳过
	}

	AActor* Avatar = ActorInfo->AvatarActor.Get();
	if (!Avatar)
	{
		return;
	}

	// 近战攻击点在角色正前方 MeleeRange 距离处
	const FVector AvatarLocation = Avatar->GetActorLocation();
	const AWeaponCharacter* WeaponCharacter = Cast<AWeaponCharacter>(Avatar);
	const FVector ForwardDir = WeaponCharacter
		? WeaponCharacter->GetAimDirection().GetSafeNormal2D()
		: Avatar->GetActorForwardVector().GetSafeNormal2D();
	const FVector HitPoint = AvatarLocation + ForwardDir * WeaponData->MeleeRange * 0.5f;

	// 对命中点施加地形伤害
	FGridDamageHelper::TryApplyTerrainDamage(
		Avatar,
		HitPoint,
		WeaponData->MeleeDamage,
		WeaponData->DamageTags);
}
