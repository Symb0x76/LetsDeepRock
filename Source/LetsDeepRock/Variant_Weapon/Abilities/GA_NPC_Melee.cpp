// Copyright Epic Games, Inc. All Rights Reserved.

#include "GA_NPC_Melee.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "GameplayEffect.h"
#include "GameFramework/Character.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "WeaponGameplayTags.h"

UGA_NPC_Melee::UGA_NPC_Melee()
{
	// NPC GA 仅服务端执行（NPC 不需要本地预测）
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;

	// 同一时间只能有一个实例
	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;

	// 配置 Ability Tag（复用近战标识）
	AbilityTags.AddTag(TAG_Ability_Weapon_Melee);

	// 死亡时阻断
	ActivationBlockedTags.AddTag(TAG_State_Dead);
}

void UGA_NPC_Melee::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	// CommitAbility：检查 Cost / Cooldown（当前 NPC 无 Cost / Cooldown GE 也可通过）
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	// 仅服务端执行 Sweep 伤害判定
	if (HasAuthority(&ActivationInfo))
	{
		ServerMeleeSweep(ActorInfo);
	}

	// 单次攻击：立即结束
	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

void UGA_NPC_Melee::ServerMeleeSweep(const FGameplayAbilityActorInfo* ActorInfo)
{
	AActor* Avatar = ActorInfo->AvatarActor.Get();
	if (!Avatar)
	{
		return;
	}

	const FVector AvatarLocation = Avatar->GetActorLocation();
	const FVector ForwardDir = Avatar->GetActorForwardVector().GetSafeNormal2D();

	// 球形重叠检测
	TArray<FOverlapResult> Overlaps;
	FCollisionShape SweepShape = FCollisionShape::MakeSphere(MeleeRange);
	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(Avatar);

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

	// 角度阈值
	const float MinDot = FMath::Cos(FMath::DegreesToRadians(MeleeAngle));

	// HitActors 去重
	TSet<AActor*> HitActors;

	for (const FOverlapResult& Overlap : Overlaps)
	{
		AActor* HitActor = Overlap.GetActor();
		if (!HitActor || HitActor == Avatar)
		{
			continue;
		}

		if (HitActors.Contains(HitActor))
		{
			continue;
		}

		// 距离校验（2D）
		const FVector ToTargetVector = HitActor->GetActorLocation() - AvatarLocation;
		const float Distance2D = ToTargetVector.Size2D();
		if (Distance2D > MeleeRange)
		{
			continue;
		}

		// 扇形角度过滤
		const FVector ToTarget = ToTargetVector.GetSafeNormal2D();
		const float DotProduct = FVector::DotProduct(ForwardDir, ToTarget);
		if (DotProduct < MinDot)
		{
			continue;
		}

		// 施加伤害
		HitActors.Add(HitActor);
		ApplyDamageToTarget(Avatar, HitActor);
	}
}

void UGA_NPC_Melee::ApplyDamageToTarget(AActor* SourceActor, AActor* TargetActor)
{
	if (!SourceActor || !TargetActor || !DamageEffectClass)
	{
		return;
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

	// 获取 Source ASC（NPC 自己）
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

	// 构建 EffectContext
	FGameplayEffectContextHandle Context = SourceASC->MakeEffectContext();
	Context.AddSourceObject(SourceActor);

	// 创建 GE Spec
	FGameplayEffectSpecHandle SpecHandle = SourceASC->MakeOutgoingSpec(
		DamageEffectClass, 1.0f, Context);

	if (!SpecHandle.IsValid())
	{
		return;
	}

	// 设置 SetByCaller 伤害值
	SpecHandle.Data->SetSetByCallerMagnitude(TAG_Data_Damage, MeleeDamage);

	// 施加伤害
	SourceASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);
}
