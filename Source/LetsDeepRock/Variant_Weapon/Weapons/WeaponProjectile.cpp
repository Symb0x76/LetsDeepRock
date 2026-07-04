// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponProjectile.h"
#include "Components/SphereComponent.h"
#include "GameFramework/ProjectileMovementComponent.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "AbilitySystemBlueprintLibrary.h"
#include "GameplayEffectTypes.h"
#include "WeaponGameplayTags.h"

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
	CollisionSphere->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Block);

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

	// 绑定碰撞事件
	if (CollisionSphere)
	{
		CollisionSphere->OnComponentBeginOverlap.AddDynamic(this, &AWeaponProjectile::OnSphereBeginOverlap);
	}

	if (ProjectileMovement)
	{
		ProjectileMovement->OnProjectileStop.AddDynamic(this, &AWeaponProjectile::OnProjectileStop);
	}

	// 使用 Native Tag 代替字符串查找
	DamageTag = TAG_Data_Damage;

	// 忽略射击者自身
	if (InstigatorActor.IsValid())
	{
		CollisionSphere->IgnoreActorWhenMoving(InstigatorActor.Get(), true);
	}
}

void AWeaponProjectile::InitDamageParams(TSubclassOf<UGameplayEffect> InDamageGE, float InDamage, AActor* InInstigator)
{
	DamageEffectClass = InDamageGE;
	DamageValue = InDamage;
	InstigatorActor = InInstigator;

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

void AWeaponProjectile::OnSphereBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
	bool bFromSweep, const FHitResult& SweepResult)
{
	// 不打自己
	if (!OtherActor || OtherActor == GetOwner() || OtherActor == GetInstigator())
	{
		return;
	}

	// 只在服务端处理伤害和销毁（投射物是复制 Actor，由服务端管理生命周期）
	if (HasAuthority())
	{
		ApplyDamageToTarget(OtherActor);
		Destroy();
	}
}

void AWeaponProjectile::OnProjectileStop(const FHitResult& ImpactResult)
{
	// 只在服务端销毁（客户端的复制 Actor 会随服务端销毁自动清除）
	if (HasAuthority())
	{
		Destroy();
	}
}

void AWeaponProjectile::ApplyDamageToTarget(AActor* TargetActor)
{
	if (!DamageEffectClass || DamageValue <= 0.0f)
	{
		return;
	}

	// 通过 IAbilitySystemInterface 获取目标 ASC（统一入口，不 Cast 具体类）
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

	// 创建 GE Spec
	FGameplayEffectSpecHandle SpecHandle = SpecOwnerASC->MakeOutgoingSpec(DamageEffectClass, 1.0f, Context);
	if (!SpecHandle.IsValid())
	{
		return;
	}

	// 注入 SetByCaller 伤害值
	UAbilitySystemBlueprintLibrary::AssignTagSetByCallerMagnitude(SpecHandle, DamageTag, DamageValue);

	// 对目标应用伤害（从 Source 施加到 Target）
	SpecOwnerASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);
}
