#include "Swarm/SwarmBugPawn.h"

#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/PackageName.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr float SwarmSmoothInterpSpeed = 15.f;
	constexpr float SwarmMinNetUpdateHz = 4.f;
	constexpr FLinearColor SwarmNeutralTint(0.35f, 0.3f, 0.25f);
	constexpr FLinearColor SwarmBurningTint(1.f, 0.4f, 0.05f);
	constexpr FLinearColor SwarmFrozenTint(0.45f, 0.8f, 1.f);
}

ASwarmBugPawn::ASwarmBugPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	bReplicates = true;
	AActor::SetReplicateMovement(true);
	SetNetUpdateFrequency(SwarmConstants::NetUpdateHz);
	SetMinNetUpdateFrequency(SwarmMinNetUpdateHz);
	SetNetCullDistanceSquared(FMath::Square(SwarmConstants::NetCullDistance));

	Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Capsule"));
	Capsule->InitCapsuleSize(30.f, SwarmConstants::CollisionHalfHeight);
	Capsule->SetCollisionEnabled(ECollisionEnabled::QueryOnly);
	Capsule->SetCollisionObjectType(ECC_Pawn);
	Capsule->SetCollisionResponseToAllChannels(ECR_Ignore);
	Capsule->SetCollisionResponseToChannel(ECC_Pawn, ECR_Block);
	Capsule->SetCollisionResponseToChannel(ECC_WorldDynamic, ECR_Overlap);
	SetRootComponent(Capsule);

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	Mesh->SetupAttachment(Capsule);
	Mesh->SetCollisionEnabled(ECollisionEnabled::NoCollision);
	static ConstructorHelpers::FObjectFinder<UStaticMesh> ConeMesh(TEXT("/Engine/BasicShapes/Cone.Cone"));
	if (ConeMesh.Succeeded())
	{
		Mesh->SetStaticMesh(ConeMesh.Object);
		ConeMeshAsset = ConeMesh.Object;
	}

	// HD2 占位模型（Content 不入库，缺失时回退锥体）；已烘焙为朝 +X、枢轴在脚底、水平长度 100uu
	ArchetypeMeshes.Add(TEXT("BasicGrunt"), TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/HD2/Bugs/Static/SM_HD2_Scavenger.SM_HD2_Scavenger"))));
	ArchetypeMeshes.Add(TEXT("FastRunner"), TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/HD2/Bugs/Static/SM_HD2_Hunter.SM_HD2_Hunter"))));
	ArchetypeMeshes.Add(TEXT("Exploder"), TSoftObjectPtr<UStaticMesh>(FSoftObjectPath(TEXT("/Game/HD2/Bugs/Static/SM_HD2_Warrior.SM_HD2_Warrior"))));

	// 池内默认失活：隐藏且无碰撞，服务器激活前不会闪现
	AActor::SetActorHiddenInGame(true);
	SetActorEnableCollision(false);
}

void ASwarmBugPawn::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(ASwarmBugPawn, bActive);
	DOREPLIFETIME(ASwarmBugPawn, ArchetypeRow);
	DOREPLIFETIME(ASwarmBugPawn, StatusFlags);
	DOREPLIFETIME(ASwarmBugPawn, ActivationSerial);
}

void ASwarmBugPawn::BeginPlay()
{
	Super::BeginPlay();
	SmoothTargetLocation = GetActorLocation();
	SmoothTargetRotation = GetActorRotation();
	LastAppliedActivationSerial = ActivationSerial;

	// 服务器不 Tick：位置由池子直接写入；客户端仅激活时 Tick
	SetActorTickEnabled(!HasAuthority() && bActive);
}

void ASwarmBugPawn::Tick(float DeltaSeconds)
{
	Super::Tick(DeltaSeconds);
	if (HasAuthority() || !bActive)
	{
		return;
	}
	const FVector Current = GetActorLocation();
	if (!Current.Equals(SmoothTargetLocation, 0.1f))
	{
		SetActorLocation(FMath::VInterpTo(Current, SmoothTargetLocation, DeltaSeconds, SwarmSmoothInterpSpeed));
	}
	const FRotator CurrentRotation = GetActorRotation();
	if (!CurrentRotation.Equals(SmoothTargetRotation, 0.1f))
	{
		SetActorRotation(FMath::RInterpTo(CurrentRotation, SmoothTargetRotation, DeltaSeconds, SwarmSmoothInterpSpeed));
	}
}

void ASwarmBugPawn::PostNetReceiveLocationAndRotation()
{
	// 不调用 Super（会直接吸附位置），改为存平滑目标
	HandleReplicatedLocation();
}

void ASwarmBugPawn::HandleReplicatedLocation()
{
	const FVector Latest = FRepMovement::RebaseOntoLocalOrigin(GetReplicatedMovement().Location, this);
	const FRotator LatestRotation = GetReplicatedMovement().Rotation;
	if (ActivationSerial != LastAppliedActivationSerial)
	{
		// 槽位被回收复用：直接吸附，避免从旧位置横穿地图
		LastAppliedActivationSerial = ActivationSerial;
		SetActorLocationAndRotation(Latest, LatestRotation);
	}
	SmoothTargetLocation = Latest;
	SmoothTargetRotation = LatestRotation;
}

void ASwarmBugPawn::ReceiveSwarmDamage(const FSwarmDamageEvent& Event)
{
	if (!HasAuthority() || !bActive || !OnDamaged)
	{
		return;
	}
	OnDamaged(*this, Event);
}

void ASwarmBugPawn::ActivateBug(int32 InSlot, FName InRow, const FVector& Location, float InMaxHealth)
{
	check(HasAuthority());
	Slot = InSlot;
	MaxHealth = InMaxHealth;
	Health = InMaxHealth;

	SetNetDormancy(DORM_Awake);
	FlushNetDormancy();

	ArchetypeRow = InRow;
	StatusFlags = 0;
	++ActivationSerial;
	bActive = true;

	SetActorLocation(Location, false, nullptr, ETeleportType::TeleportPhysics);
	ForceNetUpdate();

	// 服务器/监听主机同样应用表现
	ApplyArchetypeVisuals();
	ApplyStatusVisuals();
	ApplyActiveVisuals();
}

void ASwarmBugPawn::DeactivateBug()
{
	check(HasAuthority());
	bActive = false;
	StatusFlags = 0;
	Health = 0.f;
	Slot = INDEX_NONE;
	ApplyStatusVisuals();
	ApplyActiveVisuals();

	ForceNetUpdate();
	SetNetDormancy(DORM_DormantAll);
}

void ASwarmBugPawn::SetHealth(float InHealth)
{
	Health = FMath::Clamp(InHealth, 0.f, MaxHealth);
}

void ASwarmBugPawn::SetStatusFlags(ESwarmStatusFlags InFlags)
{
	const uint8 NewFlags = static_cast<uint8>(InFlags);
	if (StatusFlags == NewFlags)
	{
		return;
	}
	StatusFlags = NewFlags;
	if (HasAuthority())
	{
		ApplyStatusVisuals();
	}
}

void ASwarmBugPawn::OnRep_Active()
{
	ApplyActiveVisuals();
	// 客户端：失活的池内虫不 Tick
	SetActorTickEnabled(bActive);
}

void ASwarmBugPawn::OnRep_ArchetypeRow()
{
	ApplyArchetypeVisuals();
}

void ASwarmBugPawn::OnRep_StatusFlags()
{
	ApplyStatusVisuals();
}

void ASwarmBugPawn::OnRep_ActivationSerial()
{
	HandleReplicatedLocation();
}

void ASwarmBugPawn::ApplyActiveVisuals()
{
	SetActorHiddenInGame(!bActive);
	SetActorEnableCollision(bActive);
}

void ASwarmBugPawn::ApplyArchetypeVisuals()
{
	const FSwarmArchetypeTuning* Tuning = SwarmFindArchetypeTuning(ArchetypeRow);
	if (!Tuning)
	{
		return;
	}
	// 碰撞为立在地面的高柱（半高远大于半径的胶囊，侧面即圆柱），模型仍是矮锥体并贴住柱底。
	// BasicShapes Cone 高 100、枢轴在中心，缩放后半高 = 50 * MeshScale。
	Capsule->SetCapsuleSize(Tuning->CollisionRadius, SwarmConstants::CollisionHalfHeight);
	Mesh->SetRelativeScale3D(FVector(Tuning->MeshScale));

	// 原型模型枢轴在脚底，直接贴住柱底；缺失时回退锥体（枢轴在中心）。
	// 在应用时按需加载：客户端首包里 OnRep_ArchetypeRow 早于 BeginPlay，不能依赖 BeginPlay 预加载。
	if (!LoadedArchetypeMeshes.Contains(ArchetypeRow))
	{
		if (const TSoftObjectPtr<UStaticMesh>* SoftMesh = ArchetypeMeshes.Find(ArchetypeRow))
		{
			UStaticMesh* Loaded = SoftMesh->Get();
			// Content 未入库的机器上资产不存在：先查包是否存在，避免每次激活都重试读盘
			if (!Loaded && FPackageName::DoesPackageExist(SoftMesh->ToSoftObjectPath().GetLongPackageName()))
			{
				Loaded = SoftMesh->LoadSynchronous();
			}
			if (Loaded)
			{
				LoadedArchetypeMeshes.Add(ArchetypeRow, Loaded);
			}
		}
	}
	const TObjectPtr<UStaticMesh>* BugMesh = LoadedArchetypeMeshes.Find(ArchetypeRow);
	UStaticMesh* NewMesh = BugMesh ? BugMesh->Get() : ConeMeshAsset.Get();
	Mesh->SetRelativeLocation(FVector(0.f, 0.f, BugMesh
		? -SwarmConstants::CollisionHalfHeight
		: -SwarmConstants::CollisionHalfHeight + 50.f * Tuning->MeshScale));

	if (Mesh->GetStaticMesh() != NewMesh)
	{
		// 槽位回收换模型：旧着色 MID 基于上个模型的材质，必须丢弃后按当前状态重建
		Mesh->SetStaticMesh(NewMesh);
		Mesh->EmptyOverrideMaterials();
		TintMaterial = nullptr;
		ApplyStatusVisuals();
	}
}

void ASwarmBugPawn::ApplyStatusVisuals()
{
	const ESwarmStatusFlags Flags = GetStatusFlags();
	FLinearColor Tint = SwarmNeutralTint;
	if (EnumHasAnyFlags(Flags, ESwarmStatusFlags::Burning))
	{
		Tint = SwarmBurningTint;
	}
	else if (EnumHasAnyFlags(Flags, ESwarmStatusFlags::Frozen))
	{
		Tint = SwarmFrozenTint;
	}

	if (!TintMaterial)
	{
		if (Flags == ESwarmStatusFlags::None)
		{
			return; // 无需为中性色创建材质实例
		}
		TintMaterial = Mesh->CreateAndSetMaterialInstanceDynamic(0);
	}
	if (TintMaterial)
	{
		TintMaterial->SetVectorParameterValue(TEXT("Color"), Tint);
	}
}
