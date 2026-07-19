// Copyright Epic Games, Inc. All Rights Reserved.

#include "GrenadeBallistics.h"
#include "WeaponGrenadeDataAsset.h"
#include "Kismet/GameplayStatics.h"
#include "Engine/World.h"

FGrenadeBallisticsInput FGrenadeBallisticsInput::FromDataAsset(const UWeaponGrenadeDataAsset* Data,
	const FVector& InSpawnLocation, const FVector2D& InAimDirection2D,
	const FVector& InTargetPoint, bool bInHasTarget, AActor* InActorToIgnore)
{
	FGrenadeBallisticsInput Input;
	Input.SpawnLocation = InSpawnLocation;
	Input.AimDirection2D = InAimDirection2D;
	Input.TargetPoint = InTargetPoint;
	Input.bHasTarget = bInHasTarget;
	Input.ActorToIgnore = InActorToIgnore;

	if (Data)
	{
		Input.LaunchAngleDeg = Data->LaunchAngleDeg;
		Input.GravityScale = Data->GravityScale;
		Input.MinThrowSpeed = Data->MinThrowSpeed;
		Input.MaxThrowSpeed = Data->MaxThrowSpeed;
		Input.MaxThrowRange = Data->MaxThrowRange;
		Input.DefaultThrowSpeed = Data->ThrowSpeed;
		Input.ProjectileRadius = Data->ProjectileCollisionRadius;
		Input.FuseSeconds = Data->FuseSeconds;
	}
	return Input;
}

// 内部：计算投掷初速度（与旧 GA_ThrowGrenade::CalculateThrowSpeed 逻辑一致）
static float CalcThrowSpeed(const FGrenadeBallisticsInput& Input, FVector& InOutTargetPoint, UWorld* World)
{
	const float Gravity = FMath::Abs(World->GetGravityZ()) * Input.GravityScale;

	// 无重力或重力极小：使用回退速度
	if (Gravity <= KINDA_SMALL_NUMBER)
	{
		return FMath::Clamp(Input.DefaultThrowSpeed, Input.MinThrowSpeed, Input.MaxThrowSpeed);
	}

	const float Theta = FMath::DegreesToRadians(Input.LaunchAngleDeg);
	const float CosTheta = FMath::Cos(Theta);
	const float TanTheta = FMath::Tan(Theta);

	// 仰角接近 90°（cos 趋近 0）
	if (FMath::IsNearlyZero(CosTheta))
	{
		return FMath::Clamp(Input.DefaultThrowSpeed, Input.MinThrowSpeed, Input.MaxThrowSpeed);
	}

	const float DeltaHeight = InOutTargetPoint.Z - Input.SpawnLocation.Z;
	FVector2D HorizontalVec(InOutTargetPoint.X - Input.SpawnLocation.X, InOutTargetPoint.Y - Input.SpawnLocation.Y);
	float D = HorizontalVec.Size();

	// 距离过近
	if (D <= KINDA_SMALL_NUMBER)
	{
		return FMath::Clamp(Input.DefaultThrowSpeed, Input.MinThrowSpeed, Input.MaxThrowSpeed);
	}

	// 超出最大射程：裁剪目标 XY 到 MaxThrowRange，保留原 Z
	if (Input.MaxThrowRange > KINDA_SMALL_NUMBER && D > Input.MaxThrowRange)
	{
		const FVector2D ClampedVec = HorizontalVec.GetSafeNormal() * Input.MaxThrowRange;
		InOutTargetPoint.X = Input.SpawnLocation.X + ClampedVec.X;
		InOutTargetPoint.Y = Input.SpawnLocation.Y + ClampedVec.Y;
		D = Input.MaxThrowRange;
	}

	const float Denominator = D * TanTheta - DeltaHeight;
	if (Denominator <= KINDA_SMALL_NUMBER)
	{
		return FMath::Clamp(Input.DefaultThrowSpeed, Input.MinThrowSpeed, Input.MaxThrowSpeed);
	}

	const float CosThetaSq = CosTheta * CosTheta;
	const float VSquared = Gravity * D * D / (2.0f * CosThetaSq * Denominator);

	if (VSquared <= 0.0f || !FMath::IsFinite(VSquared))
	{
		return FMath::Clamp(Input.DefaultThrowSpeed, Input.MinThrowSpeed, Input.MaxThrowSpeed);
	}

	const float V = FMath::Sqrt(VSquared);
	return FMath::Clamp(V, Input.MinThrowSpeed, Input.MaxThrowSpeed);
}

// 内部：构建世界空间初速度向量
static FVector BuildInitialVelocity(const FGrenadeBallisticsInput& Input, float Speed)
{
	const FVector ForwardDir(Input.AimDirection2D.X, Input.AimDirection2D.Y, 0.0f);
	const float CosTheta = FMath::Cos(FMath::DegreesToRadians(Input.LaunchAngleDeg));
	const float SinTheta = FMath::Sin(FMath::DegreesToRadians(Input.LaunchAngleDeg));

	return FVector(
		ForwardDir.X * Speed * CosTheta,
		ForwardDir.Y * Speed * CosTheta,
		Speed * SinTheta
	);
}

bool FGrenadeBallistics::Calculate(const FGrenadeBallisticsInput& Input, UWorld* World, FGrenadeBallisticsOutput& Out)
{
	Out.PathPoints.Reset();
	Out.ExplosionPoint = FVector::ZeroVector;
	Out.bHitBlocking = false;
	Out.HitNormal = FVector::ZeroVector;

	if (!World)
	{
		// 回退：基本速度，无路径
		Out.InitialVelocity = BuildInitialVelocity(Input, Input.DefaultThrowSpeed);
		Out.ActualThrowSpeed = Input.DefaultThrowSpeed;
		Out.ExplosionPoint = Input.SpawnLocation;
		return false;
	}

	// ── 第1步：计算投掷速度 ──
	float ThrowSpeed;
	FVector EffectiveTargetPoint = Input.TargetPoint;

	if (Input.bHasTarget)
	{
		ThrowSpeed = CalcThrowSpeed(Input, EffectiveTargetPoint, World);
	}
	else
	{
		ThrowSpeed = FMath::Clamp(Input.DefaultThrowSpeed, Input.MinThrowSpeed, Input.MaxThrowSpeed);
	}

	const FVector InitialVelocity = BuildInitialVelocity(Input, ThrowSpeed);

	Out.InitialVelocity = InitialVelocity;
	Out.ActualThrowSpeed = ThrowSpeed;

	// ── 第2步：PredictProjectilePath ──
	FPredictProjectilePathParams PredictParams;
	PredictParams.StartLocation = Input.SpawnLocation;
	PredictParams.LaunchVelocity = InitialVelocity;
	PredictParams.ProjectileRadius = Input.ProjectileRadius;
	PredictParams.MaxSimTime = Input.FuseSeconds;
	PredictParams.bTraceWithCollision = true;
	PredictParams.bTraceWithChannel = true;
	PredictParams.TraceChannel = ECC_WorldStatic;
	PredictParams.DrawDebugType = EDrawDebugTrace::None;
	PredictParams.SimFrequency = 30.0f;
	PredictParams.OverrideGravityZ = World->GetGravityZ() * Input.GravityScale;

	if (Input.ActorToIgnore)
	{
		PredictParams.ActorsToIgnore.Add(Input.ActorToIgnore);
	}

	FPredictProjectilePathResult PredictResult;
	const bool bHit = UGameplayStatics::PredictProjectilePath(World, PredictParams, PredictResult);

	// 提取路径点
	for (const FPredictProjectilePathPointData& PointData : PredictResult.PathData)
	{
		Out.PathPoints.Add(PointData.Location);
	}

	// 确定爆炸点
	if (bHit && PredictResult.HitResult.bBlockingHit)
	{
		Out.ExplosionPoint = PredictResult.HitResult.ImpactPoint;
		Out.bHitBlocking = true;
		Out.HitNormal = PredictResult.HitResult.ImpactNormal;
	}
	else if (Out.PathPoints.Num() > 0)
	{
		// 引信到期时还在空中：取最后一个路径点
		Out.ExplosionPoint = Out.PathPoints.Last();
		Out.bHitBlocking = false;
	}
	else
	{
		// 回退：Spawn 位置
		Out.ExplosionPoint = Input.SpawnLocation;
		Out.bHitBlocking = false;
	}

	return true;
}
