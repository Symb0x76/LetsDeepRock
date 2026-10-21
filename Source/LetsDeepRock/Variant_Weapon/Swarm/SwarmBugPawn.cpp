#include "Swarm/SwarmBugPawn.h"

#include "Components/CapsuleComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr float SwarmSmoothInterpSpeed = 15.f;
	constexpr float SwarmMinNetUpdateHz = 4.f;
	const FLinearColor SwarmNeutralTint(0.35f, 0.3f, 0.25f);
	const FLinearColor SwarmBurningTint(1.f, 0.4f, 0.05f);
	const FLinearColor SwarmFrozenTint(0.45f, 0.8f, 1.f);
}

ASwarmBugPawn::ASwarmBugPawn()
{
	PrimaryActorTick.bCanEverTick = true;

	bReplicates = true;
	SetReplicateMovement(true);
	SetNetUpdateFrequency(SwarmConstants::NetUpdateHz);
	SetMinNetUpdateFrequency(SwarmMinNetUpdateHz);
	SetNetCullDistanceSquared(FMath::Square(SwarmConstants::NetCullDistance));

	Capsule = CreateDefaultSubobject<UCapsuleComponent>(TEXT("Capsule"));
	Capsule->InitCapsuleSize(30.f, 40.f);
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
	}

	// 池内默认失活：隐藏且无碰撞，服务器激活前不会闪现
	SetActorHiddenInGame(true);
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
	LastAppliedActivationSerial = ActivationSerial;
	// 服务器不 Tick：位置由池子直接写入
	SetActorTickEnabled(!HasAuthority());
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
}

void ASwarmBugPawn::PostNetReceiveLocationAndRotation()
{
	// 不调用 Super（会直接吸附位置），改为存平滑目标
	HandleReplicatedLocation();
}

void ASwarmBugPawn::HandleReplicatedLocation()
{
	const FVector Latest = FRepMovement::RebaseOntoLocalOrigin(GetReplicatedMovement().Location, this);
	if (ActivationSerial != LastAppliedActivationSerial)
	{
		// 槽位被回收复用：直接吸附，避免从旧位置横穿地图
		LastAppliedActivationSerial = ActivationSerial;
		SetActorLocation(Latest);
	}
	SmoothTargetLocation = Latest;
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
	Mesh->SetRelativeScale3D(FVector(Tuning->MeshScale));
	Capsule->SetCapsuleSize(Tuning->CollisionRadius, FMath::Max(Tuning->CollisionRadius, 40.f));
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
