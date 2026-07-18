// Copyright Epic Games, Inc. All Rights Reserved.

#include "GrenadeTrajectoryPreviewComponent.h"
#include "WeaponGrenadeDataAsset.h"
#include "Components/InstancedStaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "GameFramework/Pawn.h"

UGrenadeTrajectoryPreviewComponent::UGrenadeTrajectoryPreviewComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(false); // 纯本地，不复制

	// ★ 延迟创建时（ThrowGrenadeStarted 中 RegisterComponent）也能触发 InitializeComponent
	bWantsInitializeComponent = true;
}

void UGrenadeTrajectoryPreviewComponent::InitializeComponent()
{
	Super::InitializeComponent();

	// 仅在本地控制 Pawn 上创建 ISM
	AActor* Owner = GetOwner();
	APawn* PawnOwner = Cast<APawn>(Owner);
	if (!PawnOwner || !PawnOwner->IsLocallyControlled())
	{
		return;
	}

	// 运行时加载引擎内置 Sphere 网格（安全查找，缺失时无崩溃）
	SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere"));
	if (!SphereMesh)
	{
		// 备用路径
		SphereMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	}
	if (!SphereMesh)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GrenadeTrajectoryPreviewComponent] 无法加载引擎内置 Sphere 网格，预览将不可见。"));
		return;
	}

	// 创建轨迹点 ISM
	TrajectoryISM = NewObject<UInstancedStaticMeshComponent>(Owner, TEXT("TrajectoryPreviewISM"));
	if (TrajectoryISM)
	{
		TrajectoryISM->RegisterComponent();
		TrajectoryISM->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
		TrajectoryISM->SetStaticMesh(SphereMesh);
		TrajectoryISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		TrajectoryISM->SetCastShadow(false);
		TrajectoryISM->SetVisibility(false);
	}

	// 创建爆炸范围 ISM
	ExplosionPreviewISM = NewObject<UInstancedStaticMeshComponent>(Owner, TEXT("ExplosionPreviewISM"));
	if (ExplosionPreviewISM)
	{
		ExplosionPreviewISM->RegisterComponent();
		ExplosionPreviewISM->AttachToComponent(Owner->GetRootComponent(), FAttachmentTransformRules::KeepRelativeTransform);
		ExplosionPreviewISM->SetStaticMesh(SphereMesh);
		ExplosionPreviewISM->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		ExplosionPreviewISM->SetCastShadow(false);
		ExplosionPreviewISM->SetVisibility(false);
	}
}

void UGrenadeTrajectoryPreviewComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 销毁 ISM 组件（随 Owner 销毁时自动清理，此处显式确保）
	if (TrajectoryISM)
	{
		TrajectoryISM->DestroyComponent();
		TrajectoryISM = nullptr;
	}
	if (ExplosionPreviewISM)
	{
		ExplosionPreviewISM->DestroyComponent();
		ExplosionPreviewISM = nullptr;
	}
	Super::EndPlay(EndPlayReason);
}

void UGrenadeTrajectoryPreviewComponent::ShowPreview(const FGrenadeBallisticsOutput& BallisticsResult, const UWeaponGrenadeDataAsset* Data)
{
	if (!SphereMesh)
	{
		return;
	}

	ClearAllInstances();

	// 1. 放置轨迹点
	PlaceTrajectoryPoints(BallisticsResult.PathPoints);

	// 2. 放置爆炸范围轮廓
	if (Data)
	{
		switch (Data->ExplosionShape)
		{
		case EExplosionShape::Sphere:
		case EExplosionShape::Capsule:
			// Sphere 和 Capsule 的 XY 投影均为圆形
			PlaceExplosionCircle(BallisticsResult.ExplosionPoint, Data->ExplosionRadius);
			break;
		case EExplosionShape::Box:
			{
				// Box 形状：矩形
				const FVector2D HalfExtents(
					(Data->BoxExtent.SizeSquared() > KINDA_SMALL_NUMBER) ? Data->BoxExtent.X : Data->ExplosionRadius,
					(Data->BoxExtent.SizeSquared() > KINDA_SMALL_NUMBER) ? Data->BoxExtent.Y : Data->ExplosionRadius
				);
				PlaceExplosionRect(BallisticsResult.ExplosionPoint, HalfExtents);
			}
			break;
		default:
			PlaceExplosionCircle(BallisticsResult.ExplosionPoint, Data->ExplosionRadius);
			break;
		}
	}

	// 显示
	if (TrajectoryISM) TrajectoryISM->SetVisibility(true);
	if (ExplosionPreviewISM) ExplosionPreviewISM->SetVisibility(true);
	bPreviewVisible = true;
}

void UGrenadeTrajectoryPreviewComponent::UpdatePreview(const FGrenadeBallisticsOutput& BallisticsResult, const UWeaponGrenadeDataAsset* Data)
{
	// Update 与 Show 实现相同：清除旧实例后重建
	ShowPreview(BallisticsResult, Data);
}

void UGrenadeTrajectoryPreviewComponent::HidePreview()
{
	ClearAllInstances();
	if (TrajectoryISM) TrajectoryISM->SetVisibility(false);
	if (ExplosionPreviewISM) ExplosionPreviewISM->SetVisibility(false);
	bPreviewVisible = false;
}

void UGrenadeTrajectoryPreviewComponent::ClearAllInstances()
{
	if (TrajectoryISM) TrajectoryISM->ClearInstances();
	if (ExplosionPreviewISM) ExplosionPreviewISM->ClearInstances();
}

void UGrenadeTrajectoryPreviewComponent::PlaceTrajectoryPoints(const TArray<FVector>& PathPoints)
{
	if (!TrajectoryISM || !SphereMesh || PathPoints.Num() == 0)
	{
		return;
	}

	// 沿路径均匀取点（如果点太密则跳过，跟踪上一个已显示点以避免累积误差）
	const FTransform MarkerTransform(
		FQuat::Identity,
		FVector::ZeroVector,
		FVector(MarkerScale)
	);

	FVector LastDisplayedPoint = PathPoints[0];
	// 始终显示第一个点
	{
		FTransform InstanceTransform = MarkerTransform;
		InstanceTransform.SetLocation(PathPoints[0]);
		TrajectoryISM->AddInstanceWorldSpace(InstanceTransform);
	}

	for (int32 i = 1; i < PathPoints.Num(); ++i)
	{
		// 按点间距稀疏化：比较当前点与上一个已显示点的距离
		if (PointSpacing > KINDA_SMALL_NUMBER)
		{
			const float Dist = FVector::Dist(PathPoints[i], LastDisplayedPoint);
			if (Dist < PointSpacing)
			{
				continue;
			}
		}

		FTransform InstanceTransform = MarkerTransform;
		InstanceTransform.SetLocation(PathPoints[i]);
		TrajectoryISM->AddInstanceWorldSpace(InstanceTransform);
		LastDisplayedPoint = PathPoints[i];
	}

	// 确保最后一个路径点始终显示（防止稀疏化漏掉终点）
	if (PathPoints.Num() > 1)
	{
		const float DistToLast = FVector::Dist(PathPoints.Last(), LastDisplayedPoint);
		if (DistToLast > PointSpacing * 0.3f)
		{
			FTransform InstanceTransform = MarkerTransform;
			InstanceTransform.SetLocation(PathPoints.Last());
			TrajectoryISM->AddInstanceWorldSpace(InstanceTransform);
		}
	}
}

void UGrenadeTrajectoryPreviewComponent::PlaceExplosionCircle(const FVector& Center, float Radius)
{
	if (!ExplosionPreviewISM || !SphereMesh || Radius <= 0.0f)
	{
		return;
	}

	const int32 PointCount = FMath::Max(RangePointCount, 8);
	const FTransform MarkerTransform(
		FQuat::Identity,
		FVector::ZeroVector,
		FVector(MarkerScale * 1.5f) // 爆炸范围标记稍大
	);

	for (int32 i = 0; i < PointCount; ++i)
	{
		const float Angle = (2.0f * UE_PI * i) / PointCount;
		const FVector Offset(FMath::Cos(Angle) * Radius, FMath::Sin(Angle) * Radius, 0.0f);
		const FVector PointLocation = Center + Offset;

		FTransform InstanceTransform = MarkerTransform;
		InstanceTransform.SetLocation(PointLocation);
		ExplosionPreviewISM->AddInstanceWorldSpace(InstanceTransform);
	}
}

void UGrenadeTrajectoryPreviewComponent::PlaceExplosionRect(const FVector& Center, const FVector2D& HalfExtents)
{
	if (!ExplosionPreviewISM || !SphereMesh)
	{
		return;
	}

	const float HalfX = HalfExtents.X;
	const float HalfY = HalfExtents.Y;
	if (HalfX <= 0.0f && HalfY <= 0.0f)
	{
		return;
	}

	// 矩形每条边的点数
	const int32 PointsPerSide = FMath::Max(RangePointCount / 4, 4);
	const FTransform MarkerTransform(
		FQuat::Identity,
		FVector::ZeroVector,
		FVector(MarkerScale * 1.5f)
	);

	// 四条边
	for (int32 Side = 0; Side < 4; ++Side)
	{
		for (int32 j = 0; j < PointsPerSide; ++j)
		{
			const float T = (float)j / (float)(PointsPerSide - 1);
			FVector Offset(0.0f);

			switch (Side)
			{
			case 0: // 底边 Y = -HalfY
				Offset = FVector(FMath::Lerp(-HalfX, HalfX, T), -HalfY, 0.0f);
				break;
			case 1: // 右边 X = +HalfX
				Offset = FVector(HalfX, FMath::Lerp(-HalfY, HalfY, T), 0.0f);
				break;
			case 2: // 顶边 Y = +HalfY
				Offset = FVector(FMath::Lerp(HalfX, -HalfX, T), HalfY, 0.0f);
				break;
			case 3: // 左边 X = -HalfX
				Offset = FVector(-HalfX, FMath::Lerp(HalfY, -HalfY, T), 0.0f);
				break;
			}

			FTransform InstanceTransform = MarkerTransform;
			InstanceTransform.SetLocation(Center + Offset);
			ExplosionPreviewISM->AddInstanceWorldSpace(InstanceTransform);
		}
	}
}
