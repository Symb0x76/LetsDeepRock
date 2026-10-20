// Copyright Epic Games, Inc. All Rights Reserved.

#include "GridMapObjectiveActors.h"

#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Net/UnrealNetwork.h"
#include "Variant_Weapon/WeaponGameMode.h"
#include "Variant_Weapon/WeaponGameState.h"

namespace
{
	// 与 GridMapRenderer 的 fallback 染色同一套参数名,兼容引擎基础材质。
	void TintMesh(UStaticMeshComponent* MeshComponent, const FLinearColor& Color)
	{
		if (!MeshComponent)
		{
			return;
		}
		UMaterialInstanceDynamic* DynamicMaterial = MeshComponent->CreateAndSetMaterialInstanceDynamic(0);
		if (DynamicMaterial)
		{
			DynamicMaterial->SetVectorParameterValue(TEXT("BaseColor"), Color);
			DynamicMaterial->SetVectorParameterValue(TEXT("Color"), Color);
		}
	}

	// 屏障期间所有目标/撤离逻辑冻结(与再生/状态效果同一语义)。
	bool IsGameplayFrozen(UWorld* World)
	{
		return AWeaponGameState::IsGameplayRecoveryBlockedFor(World);
	}

	/** 半径内的存活玩家 Pawn 数(只遍历 PlayerController:AI 敌人不算玩家) */
	int32 CountPlayersInRadius(const UWorld* World, const FVector& Center, const float Radius)
	{
		int32 Count = 0;
		const float RadiusSquared = Radius * Radius;
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			const AController* Controller = It->Get();
			const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
			if (Pawn && !Pawn->IsPendingKillPending() &&
				FVector::DistSquared2D(Pawn->GetActorLocation(), Center) <= RadiusSquared)
			{
				++Count;
			}
		}
		return Count;
	}

	int32 CountAlivePlayers(const UWorld* World)
	{
		int32 Count = 0;
		for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
		{
			const AController* Controller = It->Get();
			if (Controller && Controller->GetPawn())
			{
				++Count;
			}
		}
		return FMath::Max(Count, 1);
	}
}

// ============================================================================
// AGridMapObjectiveActor
// ============================================================================

AGridMapObjectiveActor::AGridMapObjectiveActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.1f;
	bReplicates = true;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderMesh.Succeeded())
	{
		Mesh->SetStaticMesh(CylinderMesh.Object);
	}
	Mesh->SetWorldScale3D(FVector(1.5f, 1.5f, 0.4f));
}

void AGridMapObjectiveActor::BeginPlay()
{
	Super::BeginPlay();
	TintMesh(Mesh, bComplete ? FLinearColor::Green : FLinearColor::Yellow);
}

void AGridMapObjectiveActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AGridMapObjectiveActor, bComplete);
}

void AGridMapObjectiveActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	UWorld* World = GetWorld();
	if (!World || !HasAuthority() || bComplete || IsGameplayFrozen(World))
	{
		return;
	}

	if (CountPlayersInRadius(World, GetActorLocation(), InteractRadius) > 0)
	{
		ChargeProgress = FMath::Min(ChargeProgress + DeltaTime, ChargeSeconds);
		if (ChargeProgress >= ChargeSeconds)
		{
			SetComplete();
		}
	}
}

void AGridMapObjectiveActor::SetComplete()
{
	if (bComplete)
	{
		return;
	}
	bComplete = true;
	TintMesh(Mesh, FLinearColor::Green);

	if (UWorld* World = GetWorld())
	{
		if (AWeaponGameMode* GameMode = World->GetAuthGameMode<AWeaponGameMode>())
		{
			GameMode->HandleObjectiveCompleted();
		}
	}
}

void AGridMapObjectiveActor::RestoreProgress(const float InChargeProgress, const bool bInComplete)
{
	if (!HasAuthority())
	{
		return;
	}
	ChargeProgress = FMath::Clamp(InChargeProgress, 0.0f, ChargeSeconds);
	bComplete = bInComplete;
	TintMesh(Mesh, bComplete ? FLinearColor::Green : FLinearColor::Yellow);
}

void AGridMapObjectiveActor::OnRep_Complete()
{
	TintMesh(Mesh, bComplete ? FLinearColor::Green : FLinearColor::Yellow);
}

// ============================================================================
// AGridMapExtractionActor
// ============================================================================

AGridMapExtractionActor::AGridMapExtractionActor()
{
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.TickInterval = 0.1f;
	bReplicates = true;

	Mesh = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("Mesh"));
	RootComponent = Mesh;
	static ConstructorHelpers::FObjectFinder<UStaticMesh> CylinderMesh(
		TEXT("/Engine/BasicShapes/Cylinder.Cylinder"));
	if (CylinderMesh.Succeeded())
	{
		Mesh->SetStaticMesh(CylinderMesh.Object);
	}
	Mesh->SetWorldScale3D(FVector(2.0f, 2.0f, 0.2f));
}

void AGridMapExtractionActor::BeginPlay()
{
	Super::BeginPlay();
	// 撤离区从开局就可见(灰色=未开启),玩家能提前知道撤离点;开启后变绿。
	SetActorEnableCollision(false);
	TintMesh(Mesh, bActive ? FLinearColor::Green : FLinearColor::Gray);
}

void AGridMapExtractionActor::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AGridMapExtractionActor, bActive);
}

void AGridMapExtractionActor::SetActive()
{
	if (!HasAuthority() || bActive)
	{
		return;
	}
	bActive = true;
	TintMesh(Mesh, FLinearColor::Green);
}

void AGridMapExtractionActor::RestoreState(const bool bInActive, const float InHoldProgress)
{
	if (!HasAuthority())
	{
		return;
	}
	bActive = bInActive;
	HoldProgress = FMath::Clamp(InHoldProgress, 0.0f, HoldSeconds);
	TintMesh(Mesh, bActive ? FLinearColor::Green : FLinearColor::Gray);
}

void AGridMapExtractionActor::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	UWorld* World = GetWorld();
	if (!World || !HasAuthority() || !bActive || IsGameplayFrozen(World))
	{
		return;
	}

	// 撤离判定:所有存活玩家都在圈内才累计;有人离开即回落。
	const int32 PlayersInZone = CountPlayersInRadius(World, GetActorLocation(), ExtractRadius);
	const int32 AlivePlayers = CountAlivePlayers(World);
	if (PlayersInZone >= AlivePlayers)
	{
		HoldProgress = FMath::Min(HoldProgress + DeltaTime, HoldSeconds);
		if (HoldProgress >= HoldSeconds)
		{
			SetActorTickEnabled(false);
			if (AWeaponGameMode* GameMode = World->GetAuthGameMode<AWeaponGameMode>())
			{
				GameMode->EndWeaponMatch();
			}
		}
	}
	else
	{
		HoldProgress = FMath::Max(HoldProgress - DeltaTime, 0.0f);
	}
}

void AGridMapExtractionActor::OnRep_Active()
{
	TintMesh(Mesh, bActive ? FLinearColor::Green : FLinearColor::Gray);
}
