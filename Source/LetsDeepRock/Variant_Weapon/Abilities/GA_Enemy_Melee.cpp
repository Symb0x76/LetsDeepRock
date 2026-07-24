// Copyright Epic Games, Inc. All Rights Reserved.

#include "GA_Enemy_Melee.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "GameplayEffect.h"
#include "GameFramework/Character.h"
#include "Engine/World.h"
#include "Engine/OverlapResult.h"
#include "WeaponGameplayTags.h"

UGA_Enemy_Melee::UGA_Enemy_Melee()
{
	// Enemy GA 仅服务端执行
	NetExecutionPolicy = EGameplayAbilityNetExecutionPolicy::ServerOnly;

	InstancingPolicy = EGameplayAbilityInstancingPolicy::InstancedPerActor;

	SetAssetTags(FGameplayTagContainer(TAG_Ability_Weapon_Melee));

	ActivationBlockedTags.AddTag(TAG_State_Dead);
}

void UGA_Enemy_Melee::ActivateAbility(const FGameplayAbilitySpecHandle Handle,
	const FGameplayAbilityActorInfo* ActorInfo,
	const FGameplayAbilityActivationInfo ActivationInfo,
	const FGameplayEventData* TriggerEventData)
{
	if (!CommitAbility(Handle, ActorInfo, ActivationInfo))
	{
		EndAbility(Handle, ActorInfo, ActivationInfo, true, true);
		return;
	}

	if (HasAuthority(&ActivationInfo))
	{
		ServerMeleeSweep(ActorInfo);
	}

	EndAbility(Handle, ActorInfo, ActivationInfo, true, false);
}

void UGA_Enemy_Melee::ServerMeleeSweep(const FGameplayAbilityActorInfo* ActorInfo)
{
	AActor* Avatar = ActorInfo->AvatarActor.Get();
	if (!Avatar)
	{
		return;
	}

	const FVector AvatarLocation = Avatar->GetActorLocation();
	const FVector ForwardDir = Avatar->GetActorForwardVector().GetSafeNormal2D();

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

	const float MinDot = FMath::Cos(FMath::DegreesToRadians(MeleeAngle));

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

		const FVector ToTargetVector = HitActor->GetActorLocation() - AvatarLocation;
		const float Distance2D = ToTargetVector.Size2D();
		if (Distance2D > MeleeRange)
		{
			continue;
		}

		const FVector ToTarget = ToTargetVector.GetSafeNormal2D();
		const float DotProduct = FVector::DotProduct(ForwardDir, ToTarget);
		if (DotProduct < MinDot)
		{
			continue;
		}

		HitActors.Add(HitActor);
		ApplyDamageToTarget(Avatar, HitActor);
	}
}

void UGA_Enemy_Melee::ApplyDamageToTarget(AActor* SourceActor, AActor* TargetActor)
{
	if (!SourceActor || !TargetActor || !DamageEffectClass)
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

	FGameplayEffectContextHandle Context = SourceASC->MakeEffectContext();
	Context.AddSourceObject(SourceActor);

	FGameplayEffectSpecHandle SpecHandle = SourceASC->MakeOutgoingSpec(
		DamageEffectClass, 1.0f, Context);

	if (!SpecHandle.IsValid())
	{
		return;
	}

	SpecHandle.Data->SetSetByCallerMagnitude(TAG_Data_Damage, MeleeDamage);

	SourceASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);
}
