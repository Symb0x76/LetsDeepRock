// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponGameplayCues.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "UObject/ConstructorHelpers.h"
#include "WeaponGameplayTags.h"

namespace WeaponCueStyle
{
	const FLinearColor ShieldHitColor(0.25f, 0.6f, 1.0f, 1.0f);
	const FLinearColor HealthHitColor(1.0f, 0.15f, 0.1f, 1.0f);
	const FLinearColor ExplosionColor(1.0f, 0.5f, 0.05f, 1.0f);
	const FLinearColor BurningColor(1.0f, 0.35f, 0.0f, 1.0f);

	/** 基础球体直径 100uu:缩放 = 半径 / 50 */
	constexpr float SphereRadiusPerScale = 50.0f;

	bool ShouldSpawnFX(const UWorld* World)
	{
		return World && World->GetNetMode() != NM_DedicatedServer;
	}
}

void WeaponGameplayCues::HandleCue(
	AActor* Self, const FGameplayTag CueTag, const EGameplayCueEvent::Type EventType, const FGameplayCueParameters& Parameters)
{
	UWorld* World = Self ? Self->GetWorld() : nullptr;
	if (!WeaponCueStyle::ShouldSpawnFX(World))
	{
		return;
	}

	if (CueTag == TAG_GameplayCue_Weapon_Hit && EventType == EGameplayCueEvent::Executed)
	{
		// NormalizedMagnitude:1 = 伤到生命,0 = 只打在护盾上;RawMagnitude = 实际结算伤害。
		const bool bHealthHit = Parameters.NormalizedMagnitude > 0.0f;
		const float Grow = FMath::Clamp(Parameters.RawMagnitude / 50.0f, 0.0f, 1.0f) * 0.35f;
		AWeaponCueFXActor::SpawnBurst(World, Parameters.Location,
			bHealthHit ? WeaponCueStyle::HealthHitColor : WeaponCueStyle::ShieldHitColor,
			0.15f, 0.2f + Grow, 0.2f);
		return;
	}

	if (CueTag == TAG_GameplayCue_Weapon_Explosion && EventType == EGameplayCueEvent::Executed)
	{
		const float EndScale = FMath::Max(Parameters.RawMagnitude, 50.0f) / WeaponCueStyle::SphereRadiusPerScale;
		AWeaponCueFXActor::SpawnBurst(World, Parameters.Location, WeaponCueStyle::ExplosionColor,
			0.3f, EndScale, 0.35f);
		return;
	}

	if (CueTag == TAG_GameplayCue_Status_Burning)
	{
		TArray<AActor*> AttachedActors;
		Self->GetAttachedActors(AttachedActors);
		AWeaponCueFXActor* Existing = nullptr;
		for (AActor* Attached : AttachedActors)
		{
			AWeaponCueFXActor* FX = Cast<AWeaponCueFXActor>(Attached);
			if (FX && FX->GetCueTag() == CueTag)
			{
				Existing = FX;
				break;
			}
		}

		// OnActive = 本端目睹施加;WhileActive = 中途加入/首次可见。两者都保证存在一个循环 FX。
		if ((EventType == EGameplayCueEvent::OnActive || EventType == EGameplayCueEvent::WhileActive) && !Existing)
		{
			AWeaponCueFXActor::SpawnLoopAttached(Self, CueTag, WeaponCueStyle::BurningColor, 0.35f);
		}
		else if (EventType == EGameplayCueEvent::Removed && Existing)
		{
			Existing->Destroy();
		}
	}
}

void WeaponGameplayCues::ExecuteHitCue(UAbilitySystemComponent* TargetASC, const FGameplayEffectContextHandle& EffectContext,
	const float AppliedShieldDamage, const float AppliedHealthDamage)
{
	AActor* Avatar = TargetASC ? TargetASC->GetAvatarActor() : nullptr;
	if (!Avatar || !Avatar->HasAuthority() || AppliedShieldDamage + AppliedHealthDamage <= 0.0f)
	{
		return;
	}

	FGameplayCueParameters Parameters;
	Parameters.EffectContext = EffectContext;
	Parameters.Instigator = EffectContext.GetOriginalInstigator();
	Parameters.RawMagnitude = AppliedShieldDamage + AppliedHealthDamage;
	Parameters.NormalizedMagnitude = AppliedHealthDamage > 0.0f ? 1.0f : 0.0f;

	// 投射物命中带 HitResult;近战/手雷/持续伤害没有,用 Avatar 胸口位置。
	const FHitResult* HitResult = EffectContext.GetHitResult();
	Parameters.Location = HitResult && !HitResult->ImpactPoint.IsNearlyZero()
		? FVector(HitResult->ImpactPoint)
		: Avatar->GetActorLocation() + FVector(0.0f, 0.0f, 40.0f);

	TargetASC->ExecuteGameplayCue(TAG_GameplayCue_Weapon_Hit, Parameters);
}

void WeaponGameplayCues::ExecuteExplosionCue(AActor* InstigatorActor, const FVector& Center, const float Radius)
{
	UAbilitySystemComponent* SourceASC = UAbilitySystemBlueprintLibrary::GetAbilitySystemComponent(InstigatorActor);
	// 投掷者已死亡/离开时 ASC 可能没有 Avatar,Cue 无处路由,直接跳过(仅丢表现,不影响伤害)。
	if (!SourceASC || !SourceASC->GetAvatarActor() || !SourceASC->GetAvatarActor()->HasAuthority())
	{
		return;
	}

	FGameplayCueParameters Parameters;
	Parameters.Location = Center;
	Parameters.RawMagnitude = Radius;
	Parameters.Instigator = InstigatorActor;
	SourceASC->ExecuteGameplayCue(TAG_GameplayCue_Weapon_Explosion, Parameters);
}

// ============================================================================
// AWeaponCueFXActor
// ============================================================================

AWeaponCueFXActor::AWeaponCueFXActor()
{
	PrimaryActorTick.bCanEverTick = true;
	bReplicates = false;
	SetCanBeDamaged(false);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	Mesh->SetCastShadow(false);
	Mesh->SetGenerateOverlapEvents(false);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> SphereMesh(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (SphereMesh.Succeeded())
	{
		Mesh->SetStaticMesh(SphereMesh.Object);
	}
}

AWeaponCueFXActor* AWeaponCueFXActor::SpawnBurst(UWorld* World, const FVector& Location, const FLinearColor& Color,
	const float InStartScale, const float InEndScale, const float InLifetime)
{
	if (!World)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWeaponCueFXActor* FX = World->SpawnActor<AWeaponCueFXActor>(Location, FRotator::ZeroRotator, Params);
	if (FX)
	{
		FX->StartScale = InStartScale;
		FX->EndScale = InEndScale;
		FX->Lifetime = FMath::Max(InLifetime, 0.05f);
		FX->SetActorScale3D(FVector(InStartScale));
		FX->ApplyColor(Color);
	}
	return FX;
}

AWeaponCueFXActor* AWeaponCueFXActor::SpawnLoopAttached(
	AActor* Parent, const FGameplayTag InCueTag, const FLinearColor& Color, const float Scale)
{
	UWorld* World = Parent ? Parent->GetWorld() : nullptr;
	if (!World)
	{
		return nullptr;
	}
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	AWeaponCueFXActor* FX = World->SpawnActor<AWeaponCueFXActor>(Parent->GetActorLocation(), FRotator::ZeroRotator, Params);
	if (FX)
	{
		FX->CueTag = InCueTag;
		FX->bLooping = true;
		FX->StartScale = Scale;
		FX->SetActorScale3D(FVector(Scale));
		FX->ApplyColor(Color);
		FX->AttachToActor(Parent, FAttachmentTransformRules::KeepWorldTransform);
		FX->SetActorRelativeLocation(FVector(0.0f, 0.0f, 120.0f));
	}
	return FX;
}

void AWeaponCueFXActor::Tick(const float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	Age += DeltaSeconds;

	if (bLooping)
	{
		// 父 Actor 销毁时附着子 Actor 只会脱离不会跟着销毁,这里自清理。
		if (!GetAttachParentActor())
		{
			Destroy();
			return;
		}
		const float Pulse = 1.0f + 0.2f * FMath::Sin(Age * 10.0f);
		SetActorScale3D(FVector(StartScale * Pulse));
		return;
	}

	const float Alpha = FMath::Clamp(Age / Lifetime, 0.0f, 1.0f);
	SetActorScale3D(FVector(FMath::Lerp(StartScale, EndScale, FMath::InterpEaseOut(0.0f, 1.0f, Alpha, 2.0f))));
	if (Alpha >= 1.0f)
	{
		Destroy();
	}
}

void AWeaponCueFXActor::ApplyColor(const FLinearColor& Color)
{
	if (!Mesh)
	{
		return;
	}
	// 白盒约定:同时写 BaseColor 与 Color,兼容 BasicShapeMaterial 与项目里的纯色材质。
	if (UMaterialInstanceDynamic* DynamicMaterial = Mesh->CreateAndSetMaterialInstanceDynamic(0))
	{
		DynamicMaterial->SetVectorParameterValue(TEXT("BaseColor"), Color);
		DynamicMaterial->SetVectorParameterValue(TEXT("Color"), Color);
	}
}
