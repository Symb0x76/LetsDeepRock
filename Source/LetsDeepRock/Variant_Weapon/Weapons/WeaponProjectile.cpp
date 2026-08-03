// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponProjectile.h"
#include "Components/SphereComponent.h"
#include "Components/StaticMeshComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "GameplayEffectTypes.h"
#include "WeaponGameplayTags.h"
#include "GridMap/GridDamageHelper.h"

AWeaponProjectile::AWeaponProjectile()
{
	PrimaryActorTick.bCanEverTick = false;

	// 复制：投射物需要从服务端复制到客户端
	bReplicates = true;

	// 生命周期：超时自动销毁
	InitialLifeSpan = 3.0f;

	// 碰撞球体
	CollisionSphere = CreateDefaultSubobject<USphereComponent>(TEXT("CollisionSphere"));
	RootComponent = CollisionSphere;
	CollisionSphere->SetSphereRadius(15.0f);
	CollisionSphere->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	CollisionSphere->SetCollisionObjectType(ECC_WorldDynamic);
	CollisionSphere->SetCollisionResponseToAllChannels(ECR_Ignore);
	CollisionSphere->SetCollisionResponseToChannel(ECC_Pawn, ECR_Overlap);
	CollisionSphere->SetCollisionResponseToChannel(ECC_WorldStatic, ECR_Block);

	// 网格体（纯视觉，无碰撞）
	ProjectileMesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("ProjectileMesh"));
	ProjectileMesh->SetupAttachment(CollisionSphere);
	ProjectileMesh->SetCollisionProfileName(FName("NoCollision"));

	// 投射物移动
	ProjectileMovement = CreateDefaultSubobject<UProjectileMovementComponent>(TEXT("ProjectileMovement"));
	ProjectileMovement->InitialSpeed = 3000.0f;
	ProjectileMovement->MaxSpeed = 3000.0f;
	ProjectileMovement->bRotationFollowsVelocity = true;
	ProjectileMovement->ProjectileGravityScale = 0.0f;
	ProjectileMovement->bShouldBounce = false;

	// DamageTag 在 BeginPlay 中从 Native Tag 初始化
}

void AWeaponProjectile::BeginPlay()
{
	Super::BeginPlay();

	// 使用 Native Tag 代替字符串查找
	DamageTag = TAG_Data_Damage;

	// 忽略射击者自身
	if (InstigatorActor.IsValid())
	{
		CollisionSphere->IgnoreActorWhenMoving(InstigatorActor.Get(), true);
	}

	// 绑定碰撞事件
	if (CollisionSphere)
	{
		CollisionSphere->OnComponentBeginOverlap.AddDynamic(this, &AWeaponProjectile::OnSphereBeginOverlap);
	}

	if (ProjectileMovement)
	{
		ProjectileMovement->OnProjectileStop.AddDynamic(this, &AWeaponProjectile::OnProjectileStop);
	}
}

void AWeaponProjectile::InitDamageParams(TSubclassOf<UGameplayEffect> InDamageGE, float InDamage, AActor* InInstigator, const FGameplayTagContainer& InDamageTags, bool bInPenetratesTerrain)
{
	DamageEffectClass = InDamageGE;
	DamageValue = InDamage;
	InstigatorActor = InInstigator;
	DamageTags = InDamageTags;
	bPenetratesTerrain = bInPenetratesTerrain;

	// 设置 Actor 的 Instigator 用于引擎层面的忽略判定
	SetInstigator(Cast<APawn>(InInstigator));
	SetOwner(InInstigator);
}

void AWeaponProjectile::SetProjectileSpeed(float InSpeed)
{
	if (ProjectileMovement && InSpeed > 0.0f)
	{
		ProjectileMovement->InitialSpeed = InSpeed;
		ProjectileMovement->MaxSpeed = InSpeed;
		// 速度已设置，需要重新初始化移动方向（Velocity = Forward * Speed）
		ProjectileMovement->SetVelocityInLocalSpace(FVector(InSpeed, 0.f, 0.f));
	}
}

void AWeaponProjectile::InitFlightParams(float InLifetime, float InGravityScale, int32 InMaxHitTargets)
{
	if (InLifetime > 0.0f)
	{
		SetLifeSpan(InLifetime);
	}
	if (ProjectileMovement)
	{
		ProjectileMovement->ProjectileGravityScale = InGravityScale;
	}
	MaxHitTargets = FMath::Max(1, InMaxHitTargets);
}

void AWeaponProjectile::OnSphereBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
	bool bFromSweep, const FHitResult& SweepResult)
{
	// 不打自己
	if (!OtherActor || OtherActor == GetOwner() || OtherActor == GetInstigator())
	{
		return;
	}

	// 只在服务端处理伤害和销毁
	if (!HasAuthority())
	{
		return;
	}

	APawn* HitPawn = Cast<APawn>(OtherActor);
	if (!HitPawn || HitPawns.Contains(HitPawn))
	{
		return;
	}

	if (bFromSweep)
	{
		PendingHitResult = SweepResult;
	}
	const bool bDamageApplied = ApplyDamageToTarget(HitPawn);
	PendingHitResult.Reset();
	if (bDamageApplied)
	{
		HitPawns.Add(HitPawn);
		if (HitPawns.Num() >= MaxHitTargets)
		{
			Destroy();
		}
	}
}

void AWeaponProjectile::OnProjectileStop(const FHitResult& ImpactResult)
{
	// 只在服务端处理
	if (!HasAuthority())
	{
		return;
	}

	// 投射物撞到 WorldStatic（地形方块）时，尝试施加地形伤害
	if (DamageValue > 0.0f)
	{
		const FVector HitLocation = ImpactResult.ImpactPoint.IsZero() ? GetActorLocation() : FVector(ImpactResult.ImpactPoint);
		FGridDamageHelper::TryApplyTerrainDamage(this, HitLocation, DamageValue, DamageTags);
	}

	// 根据穿透设置决定是否销毁
	if (bPenetratesTerrain)
	{
		// 穿透模式：恢复速度继续飞行
		if (ProjectileMovement)
		{
			const FVector Dir = GetActorForwardVector();
			ProjectileMovement->SetVelocityInLocalSpace(FVector(ProjectileMovement->MaxSpeed, 0.f, 0.f));
		}
	}
	else
	{
		Destroy();
	}
}

bool AWeaponProjectile::ApplyDamageToTarget(APawn* TargetPawn)
{
	if (!DamageEffectClass || DamageValue <= 0.0f)
	{
		return false;
	}

	// 通过 IAbilitySystemInterface 获取目标 ASC（统一入口，不 Cast 具体类）
	IAbilitySystemInterface* TargetASCInterface = Cast<IAbilitySystemInterface>(TargetPawn);
	if (!TargetASCInterface)
	{
		return false;
	}

	UAbilitySystemComponent* TargetASC = TargetASCInterface->GetAbilitySystemComponent();
	if (!TargetASC)
	{
		return false;
	}

	// 获取射击者（Source）的 ASC，用于正确的伤害归因
	UAbilitySystemComponent* SourceASC = nullptr;
	if (InstigatorActor.IsValid())
	{
		if (IAbilitySystemInterface* SourceASCInterface = Cast<IAbilitySystemInterface>(InstigatorActor.Get()))
		{
			SourceASC = SourceASCInterface->GetAbilitySystemComponent();
		}
	}

	// 优先从 Source ASC 创建 Spec（正确的 GAS 语义：伤害来源 → 目标）
	// 若射击者无 ASC（理论上不应发生），回退到目标 ASC 自建
	UAbilitySystemComponent* SpecOwnerASC = SourceASC ? SourceASC : TargetASC;

	// 构建 EffectContext：携带因果信息（谁射的、用什么射的）
	FGameplayEffectContextHandle Context = SpecOwnerASC->MakeEffectContext();
	Context.AddInstigator(InstigatorActor.Get(), this);
	if (PendingHitResult.IsSet())
	{
		Context.AddHitResult(PendingHitResult.GetValue());
	}

	// 创建 GE Spec
	FGameplayEffectSpecHandle SpecHandle = SpecOwnerASC->MakeOutgoingSpec(DamageEffectClass, 1.0f, Context);
	if (!SpecHandle.IsValid())
	{
		return false;
	}

	// 注入 SetByCaller 伤害值
	UAbilitySystemBlueprintLibrary::AssignTagSetByCallerMagnitude(SpecHandle, DamageTag, DamageValue);
	SpecHandle.Data->AppendDynamicAssetTags(DamageTags);

	// 对目标应用伤害（从 Source 施加到 Target）
	return SpecOwnerASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC).IsValid();
}

void AWeaponProjectile::ApplyDamageToTarget(AActor* TargetActor)
{
	ApplyDamageToTarget(Cast<APawn>(TargetActor));
}
