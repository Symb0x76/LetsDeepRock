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
#include "WeaponGameMode.h"
#include "WeaponGameState.h"
#include "GameFramework/Controller.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerState.h"
#include "GridMap/GridDamageHelper.h"
#include "Net/UnrealNetwork.h"
#include "Engine/OverlapResult.h"
#include "Attributes/StatusEffectComponent.h"

namespace
{
FUniqueNetIdRepl ResolveInstigatorPlayerId(const AActor* Actor)
{
	if (const APlayerState* PlayerState = Cast<APlayerState>(Actor))
	{
		return PlayerState->GetUniqueId();
	}
	if (const APawn* Pawn = Cast<APawn>(Actor))
	{
		return Pawn->GetPlayerState() ? Pawn->GetPlayerState()->GetUniqueId() : FUniqueNetIdRepl();
	}
	if (const AController* Controller = Cast<AController>(Actor))
	{
		const APlayerState* PlayerState = Controller->GetPlayerState<APlayerState>();
		return PlayerState ? PlayerState->GetUniqueId() : FUniqueNetIdRepl();
	}
	return FUniqueNetIdRepl();
}
}

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

}

void AWeaponProjectile::BeginPlay()
{
	Super::BeginPlay();

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

	if (HasAuthority())
	{
		if (AWeaponGameMode* WeaponGameMode = GetWorld()->GetAuthGameMode<AWeaponGameMode>())
		{
			WeaponGameMode->RegisterAttributionProjectile(InitializationState.InstigatorPlayerId, this);
		}
	}

	SetGameplayRecoveryBlocked(AWeaponGameState::IsGameplayRecoveryBlockedFor(this));
}

void AWeaponProjectile::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (HasAuthority())
	{
		if (AWeaponGameMode* WeaponGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AWeaponGameMode>() : nullptr)
		{
			WeaponGameMode->UnregisterAttributionProjectile(InitializationState.InstigatorPlayerId, this);
		}
	}
	Super::EndPlay(EndPlayReason);
}

void AWeaponProjectile::InitializeProjectile(TSubclassOf<UGameplayEffect> InDamageGE, float InDamage,
	AActor* InInstigator, const FGameplayTagContainer& InDamageTags,
	bool bInPenetratesTerrain, float InProjectileSpeed,
	float InDamageFalloffStart, float InDamageFalloffEnd,
	FName InStatusEffectId,
	const FUniqueNetIdRepl& ExplicitPlayerId)
{
	if (!HasAuthority())
	{
		return;
	}

	InitializationState.DamageEffectClass = InDamageGE;
	InitializationState.DamageValue = InDamage;
	InitializationState.InstigatorActor = InInstigator;
	InitializationState.InstigatorPlayerId = ExplicitPlayerId.IsValid()
		? ExplicitPlayerId
		: ResolveInstigatorPlayerId(InInstigator);
	InitializationState.DamageTags = InDamageTags;
	InitializationState.StatusEffectId = InStatusEffectId;
	InitializationState.bPenetratesTerrain = bInPenetratesTerrain;
	InitializationState.ProjectileSpeed = InProjectileSpeed > 0.0f ? InProjectileSpeed : 3000.0f;
	InitializationState.DamageFalloffStart = InDamageFalloffStart;
	InitializationState.DamageFalloffEnd = InDamageFalloffEnd;
	InitializationState.bInitialized = true;

#if !UE_BUILD_SHIPPING
	UE_LOG(LogTemp, Log, TEXT("[FalloffTrace] Projectile InitDamageParams Projectile=%s Target=None AppliedDamage=%.3f FalloffStart=%.3f FalloffEnd=%.3f PendingHitResult=%s"),
		*GetNameSafe(this),
		InDamage,
		InDamageFalloffStart,
		InDamageFalloffEnd,
		PendingHitResult.IsSet() ? TEXT("true") : TEXT("false"));
#endif

	// 设置 Actor 的 Instigator 用于引擎层面的忽略判定
	SetInstigator(Cast<APawn>(InInstigator));
	SetOwner(InInstigator);
	// ApplyInitializationState 由 FinishSpawning→BeginPlay（服务端）或 OnRep（客户端）统一触发，
	// 避免 Deferred Spawn 期间 GetActorForwardVector 读到 ExecuteConstruction 前的中间旋转。
}

FVector AWeaponProjectile::GetProjectileVelocity() const
{
	return ProjectileMovement ? ProjectileMovement->Velocity : GetVelocity();
}

void AWeaponProjectile::RestoreSnapshotMotion(const FVector& Velocity, float RemainingLifeSeconds)
{
	if (bGameplayRecoveryBlocked)
	{
		RecoveryBlockedVelocity = Velocity;
		RecoveryBlockedLifeSeconds = FMath::Max(0.0f, RemainingLifeSeconds);
		return;
	}
	if (ProjectileMovement)
	{
		ProjectileMovement->Velocity = Velocity;
	}
	if (RemainingLifeSeconds > 0.0f)
	{
		SetLifeSpan(RemainingLifeSeconds);
	}
}

void AWeaponProjectile::SetGameplayRecoveryBlocked(const bool bBlocked)
{
	if (bBlocked)
	{
		if (!bGameplayRecoveryBlocked)
		{
			bMovementWasActiveBeforeRecovery = ProjectileMovement && ProjectileMovement->IsActive();
			RecoveryBlockedVelocity = ProjectileMovement ? ProjectileMovement->Velocity : FVector::ZeroVector;
			bCollisionWasEnabledBeforeRecovery = GetActorEnableCollision();
			RecoveryBlockedLifeSeconds = GetLifeSpan();
		}
		bGameplayRecoveryBlocked = true;
		if (ProjectileMovement)
		{
			ProjectileMovement->StopMovementImmediately();
			ProjectileMovement->Deactivate();
		}
		SetActorEnableCollision(false);
		SetLifeSpan(0.0f);
		return;
	}

	if (!bGameplayRecoveryBlocked)
	{
		return;
	}
	bGameplayRecoveryBlocked = false;
	SetActorEnableCollision(bCollisionWasEnabledBeforeRecovery);
	if (RecoveryBlockedLifeSeconds > 0.0f)
	{
		SetLifeSpan(RecoveryBlockedLifeSeconds);
	}
	if (bMovementWasActiveBeforeRecovery && ProjectileMovement)
	{
		ProjectileMovement->Activate(true);
		ProjectileMovement->Velocity = RecoveryBlockedVelocity;
	}
	bMovementWasActiveBeforeRecovery = false;
	bCollisionWasEnabledBeforeRecovery = false;
	RecoveryBlockedVelocity = FVector::ZeroVector;
	RecoveryBlockedLifeSeconds = 0.0f;
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
	if (ProjectileMovement)
	{
		ProjectileMovement->ProjectileGravityScale = InitializationState.ProjectileGravityScale;
	}
	AreaDamage = InitializationState.AreaDamage;
	AreaRadius = InitializationState.AreaRadius;
	AreaFalloff = InitializationState.AreaFalloff;
	MaxHitTargets = FMath::Max(1, InitializationState.MaxHitTargets);
}

void AWeaponProjectile::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWeaponProjectile, InitializationState);
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
	InitializationState.ProjectileGravityScale = InGravityScale;
	InitializationState.MaxHitTargets = FMath::Max(1, InMaxHitTargets);
	MaxHitTargets = InitializationState.MaxHitTargets;
}

void AWeaponProjectile::InitAreaDamageParams(float InAreaDamage, float InAreaRadius, float InAreaFalloff)
{
	AreaDamage = InAreaDamage;
	AreaRadius = InAreaRadius;
	AreaFalloff = InAreaFalloff;
	InitializationState.AreaDamage = InAreaDamage;
	InitializationState.AreaRadius = InAreaRadius;
	InitializationState.AreaFalloff = InAreaFalloff;
}

void AWeaponProjectile::OnSphereBeginOverlap(UPrimitiveComponent* OverlappedComp, AActor* OtherActor,
	UPrimitiveComponent* OtherComp, int32 OtherBodyIndex,
	bool bFromSweep, const FHitResult& SweepResult)
{
	if (bGameplayRecoveryBlocked || AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}
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

	if (AreaDamage > 0.0f && AreaRadius > 0.0f)
	{
		const FVector ExplosionCenter = bFromSweep && !SweepResult.ImpactPoint.IsZero()
			? FVector(SweepResult.ImpactPoint)
			: GetActorLocation();
		Explode(ExplosionCenter, bFromSweep ? &SweepResult : nullptr, HitPawn);
		Destroy();
		return;
	}

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
	if (bGameplayRecoveryBlocked || AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}
	// 只在服务端处理
	if (!HasAuthority())
	{
		return;
	}

	const bool bIsWorldStatic = ImpactResult.GetComponent()
		&& ImpactResult.GetComponent()->GetCollisionObjectType() == ECC_WorldStatic;
	if (AreaDamage > 0.0f && AreaRadius > 0.0f && bIsWorldStatic)
	{
		const FVector ExplosionCenter = ImpactResult.ImpactPoint.IsZero() ? GetActorLocation() : FVector(ImpactResult.ImpactPoint);
		Explode(ExplosionCenter, &ImpactResult);
		Destroy();
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

bool AWeaponProjectile::ApplyDamageToTarget(APawn* TargetPawn, TOptional<float> DamageOverride)
{
	const float AppliedDamage = DamageOverride.IsSet() ? DamageOverride.GetValue() : InitializationState.DamageValue;
	if (bGameplayRecoveryBlocked || AWeaponGameState::IsGameplayRecoveryBlockedFor(this) ||
		!InitializationState.DamageEffectClass || AppliedDamage <= 0.0f)
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
	if (PendingHitResult.IsSet())
	{
		Context.AddHitResult(PendingHitResult.GetValue());
	}

	// 创建 GE Spec
	FGameplayEffectSpecHandle SpecHandle = SpecOwnerASC->MakeOutgoingSpec(
		InitializationState.DamageEffectClass, 1.0f, Context);
	if (!SpecHandle.IsValid())
	{
		return false;
	}

	// 注入 SetByCaller 伤害值
	UAbilitySystemBlueprintLibrary::AssignTagSetByCallerMagnitude(SpecHandle, TAG_Data_Damage, AppliedDamage);
	UAbilitySystemBlueprintLibrary::AssignTagSetByCallerMagnitude(SpecHandle, TAG_Data_DamageFalloffStart, InitializationState.DamageFalloffStart);
	UAbilitySystemBlueprintLibrary::AssignTagSetByCallerMagnitude(SpecHandle, TAG_Data_DamageFalloffEnd, InitializationState.DamageFalloffEnd);
	SpecHandle.Data->AppendDynamicAssetTags(InitializationState.DamageTags);

#if !UE_BUILD_SHIPPING
	UE_LOG(LogTemp, Log, TEXT("[FalloffTrace] Projectile ApplyGE Projectile=%s Target=%s AppliedDamage=%.3f FalloffStart=%.3f FalloffEnd=%.3f PendingHitResult=%s"),
		*GetNameSafe(this),
		*GetNameSafe(TargetPawn),
		AppliedDamage,
		InitializationState.DamageFalloffStart,
		InitializationState.DamageFalloffEnd,
		PendingHitResult.IsSet() ? TEXT("true") : TEXT("false"));
#endif

	// 对目标应用伤害（从 Source 施加到 Target）
	SpecOwnerASC->ApplyGameplayEffectSpecToTarget(*SpecHandle.Data.Get(), TargetASC);
	const bool bApplied = true;
	if (bApplied && !InitializationState.StatusEffectId.IsNone() && !StatusAppliedPawns.Contains(TargetPawn))
	{
		if (UStatusEffectComponent* StatusComponent = TargetPawn->FindComponentByClass<UStatusEffectComponent>())
		{
			StatusComponent->TryApply(InitializationState.StatusEffectId, InitializationState.DamageEffectClass);
			StatusAppliedPawns.Add(TargetPawn);
		}
	}
	return bApplied;
}

void AWeaponProjectile::ApplyDamageToTarget(AActor* TargetActor)
{
	ApplyDamageToTarget(Cast<APawn>(TargetActor));
}

void AWeaponProjectile::Explode(const FVector& ExplosionCenter, const FHitResult* SourceHitResult, APawn* DirectHitPawn)
{
	if (!HasAuthority() || bHasExploded || AreaDamage <= 0.0f || AreaRadius <= 0.0f)
	{
		return;
	}
	bHasExploded = true;

	FCollisionObjectQueryParams ObjectQueryParams;
	ObjectQueryParams.AddObjectTypesToQuery(ECC_Pawn);
	FCollisionQueryParams QueryParams(SCENE_QUERY_STAT(WeaponProjectileExplosion), false, this);
	QueryParams.AddIgnoredActor(this);
	QueryParams.AddIgnoredActor(GetOwner());
	QueryParams.AddIgnoredActor(GetInstigator());
	if (InitializationState.InstigatorActor)
	{
		QueryParams.AddIgnoredActor(InitializationState.InstigatorActor);
	}

	TArray<FOverlapResult> Overlaps;
	GetWorld()->OverlapMultiByObjectType(
		Overlaps,
		ExplosionCenter,
		FQuat::Identity,
		ObjectQueryParams,
		FCollisionShape::MakeSphere(AreaRadius),
		QueryParams);

	FHitResult ExplosionHit = SourceHitResult ? *SourceHitResult : FHitResult();
	ExplosionHit.Location = ExplosionCenter;
	ExplosionHit.ImpactPoint = ExplosionCenter;
	PendingHitResult = ExplosionHit;

	TSet<TWeakObjectPtr<APawn>> DamagedPawns;
	const float EdgeMultiplier = FMath::Clamp(AreaFalloff, 0.0f, 1.0f);
	for (const FOverlapResult& Overlap : Overlaps)
	{
		APawn* TargetPawn = Cast<APawn>(Overlap.GetActor());
		if (!TargetPawn || DamagedPawns.Contains(TargetPawn))
		{
			continue;
		}
		DamagedPawns.Add(TargetPawn);

		const float DistanceAlpha = FMath::Clamp(FVector::Distance(ExplosionCenter, TargetPawn->GetActorLocation()) / AreaRadius, 0.0f, 1.0f);
		const float DamageMultiplier = FMath::Lerp(1.0f, EdgeMultiplier, DistanceAlpha);
		ApplyDamageToTarget(TargetPawn, AreaDamage * DamageMultiplier);
	}

	AActor* DirectHitActor = DirectHitPawn;
	if (DirectHitPawn
		&& DirectHitActor != this
		&& DirectHitActor != GetOwner()
		&& DirectHitActor != GetInstigator()
		&& DirectHitActor != InitializationState.InstigatorActor
		&& !DamagedPawns.Contains(DirectHitPawn))
	{
		DamagedPawns.Add(DirectHitPawn);
		const float DistanceAlpha = FMath::Clamp(FVector::Distance(ExplosionCenter, DirectHitPawn->GetActorLocation()) / AreaRadius, 0.0f, 1.0f);
		const float DamageMultiplier = FMath::Lerp(1.0f, EdgeMultiplier, DistanceAlpha);
		ApplyDamageToTarget(DirectHitPawn, AreaDamage * DamageMultiplier);
	}

	PendingHitResult.Reset();
}
