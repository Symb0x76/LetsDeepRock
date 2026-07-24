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
#include "GameFramework/Pawn.h"
#include "GridMap/GridDamageHelper.h"
#include "Net/UnrealNetwork.h"

AWeaponProjectile::AWeaponProjectile()
{
	PrimaryActorTick.bCanEverTick = false;

	// 复制：投射物需要从服务端复制到客户端
	bReplicates = true;
	AActor::SetReplicateMovement(true);

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

	ApplyInitializationState();

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

void AWeaponProjectile::InitializeProjectile(TSubclassOf<UGameplayEffect> InDamageGE, float InDamage,
	AActor* InInstigator, const FGameplayTagContainer& InDamageTags,
	bool bInPenetratesTerrain, float InProjectileSpeed)
{
	if (!HasAuthority())
	{
		return;
	}

	InitializationState.DamageEffectClass = InDamageGE;
	InitializationState.DamageValue = InDamage;
	InitializationState.InstigatorActor = InInstigator;
	InitializationState.DamageTags = InDamageTags;
	InitializationState.bPenetratesTerrain = bInPenetratesTerrain;
	InitializationState.ProjectileSpeed = InProjectileSpeed > 0.0f ? InProjectileSpeed : 3000.0f;
	InitializationState.bInitialized = true;

	// 设置 Actor 的 Instigator 用于引擎层面的忽略判定
	SetInstigator(Cast<APawn>(InInstigator));
	SetOwner(InInstigator);
	// ApplyInitializationState 由 FinishSpawning→BeginPlay（服务端）或 OnRep（客户端）统一触发，
	// 避免 Deferred Spawn 期间 GetActorForwardVector 读到 ExecuteConstruction 前的中间旋转。
}

void AWeaponProjectile::OnRep_InitializationState()
{
	ApplyInitializationState();
}

void AWeaponProjectile::ApplyInitializationState()
{
	if (!InitializationState.bInitialized)
	{
		return;
	}

	if (CollisionSphere && InitializationState.InstigatorActor)
	{
		CollisionSphere->IgnoreActorWhenMoving(InitializationState.InstigatorActor, true);
	}

	if (ProjectileMovement && InitializationState.ProjectileSpeed > 0.0f)
	{
		ProjectileMovement->InitialSpeed = InitializationState.ProjectileSpeed;
		ProjectileMovement->MaxSpeed = InitializationState.ProjectileSpeed;
		// 生成阶段已经按 SpawnTransform 的朝向把局部初始速度转换到世界空间；
		// 此处若再次调用 SetVelocityInLocalSpace，会把当前朝向叠加一次，导致偏角翻倍。
		const FVector SpawnForward = GetActorForwardVector();
		ProjectileMovement->Velocity = SpawnForward * InitializationState.ProjectileSpeed;
		UE_LOG(LogTemp, Warning, TEXT("[Projectile] SpawnForward=(%.2f, %.2f) Vel=(%.2f, %.2f) RotYaw=%.1f"),
			SpawnForward.X, SpawnForward.Y, ProjectileMovement->Velocity.X, ProjectileMovement->Velocity.Y, GetActorRotation().Yaw);
	}
}

void AWeaponProjectile::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWeaponProjectile, InitializationState);
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

	// Overlap 只对 Pawn 生效（敌人命中）
	ApplyDamageToTarget(OtherActor);
	Destroy();
}

void AWeaponProjectile::OnProjectileStop(const FHitResult& ImpactResult)
{
	// 只在服务端处理
	if (!HasAuthority())
	{
		return;
	}

	// 投射物撞到 WorldStatic（地形方块）时，尝试施加地形伤害
	if (InitializationState.DamageValue > 0.0f)
	{
		const FVector HitLocation = ImpactResult.ImpactPoint.IsZero() ? GetActorLocation() : FVector(ImpactResult.ImpactPoint);
		FGridDamageHelper::TryApplyTerrainDamage(this, HitLocation,
			InitializationState.DamageValue, InitializationState.DamageTags);
	}

	// 根据穿透设置决定是否销毁
	if (InitializationState.bPenetratesTerrain)
	{
		// 穿透模式：恢复速度继续飞行
		if (ProjectileMovement)
		{
			ProjectileMovement->Velocity = GetActorForwardVector() * ProjectileMovement->MaxSpeed;
		}
	}
	else
	{
		Destroy();
	}
}

void AWeaponProjectile::ApplyDamageToTarget(AActor* TargetActor)
{
	if (!InitializationState.DamageEffectClass || InitializationState.DamageValue <= 0.0f)
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

	// 获取射击者（Source）的 ASC，用于正确伤害归因
	UAbilitySystemComponent* SourceASC = nullptr;
	if (InitializationState.InstigatorActor)
	{
		if (IAbilitySystemInterface* SourceASCInterface = Cast<IAbilitySystemInterface>(InitializationState.InstigatorActor))
		{
			SourceASC = SourceASCInterface->GetAbilitySystemComponent();
		}
	}

	// 优先从 Source ASC 创建 Spec（正确的 GAS 语义：伤害来源 → 目标）
	// 若射击者无 ASC（理论上不应发生），回退到目标 ASC 自建
	UAbilitySystemComponent* SpecOwnerASC = SourceASC ? SourceASC : TargetASC;

	// 构建 EffectContext：携带因果信息（谁射的、用什么射的）
	FGameplayEffectContextHandle Context = SpecOwnerASC->MakeEffectContext();
	Context.AddInstigator(InitializationState.InstigatorActor, this);

	// 创建 GE Spec
	FGameplayEffectSpecHandle SpecHandle = SpecOwnerASC->MakeOutgoingSpec(
		InitializationState.DamageEffectClass, 1.0f, Context);
	if (!SpecHandle.IsValid())
	{
		return;
	}

	// 注入 SetByCaller 伤害值
	UAbilitySystemBlueprintLibrary::AssignTagSetByCallerMagnitude(
		SpecHandle, DamageTag, InitializationState.DamageValue);

	// 对目标应用伤害（从 Source 施加到 Target）
	SpecOwnerASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);
}