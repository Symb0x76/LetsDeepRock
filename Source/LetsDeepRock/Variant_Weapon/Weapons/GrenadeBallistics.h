// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class UWeaponGrenadeDataAsset;
class AActor;

/**
 * 手雷弹道共享解算：输入参数
 *
 * 由客户端预览组件和服务器 GA 共用同一套计算逻辑，
 * 确保预览轨迹与实际生成弹道一致。
 */
struct FGrenadeBallisticsInput
{
	/** 手雷生成位置（世界坐标） */
	FVector SpawnLocation = FVector::ZeroVector;

	/** 2D 瞄准方向（XY 平面单位向量） */
	FVector2D AimDirection2D = FVector2D::ZeroVector;

	/** 地面目标点（鼠标地面命中点），世界坐标 */
	FVector TargetPoint = FVector::ZeroVector;

	/** 是否有有效地面目标（true=使用动态弹道解算，false=使用 DataAsset 默认 ThrowSpeed） */
	bool bHasTarget = false;

	/** 发射仰角（度） */
	float LaunchAngleDeg = 35.0f;

	/** 重力缩放（1.0 = 标准重力） */
	float GravityScale = 1.0f;

	/** 最小投掷速度（cm/s），解算结果 clamp 下限 */
	float MinThrowSpeed = 1.0f;

	/** 最大投掷速度（cm/s），解算结果 clamp 上限 */
	float MaxThrowSpeed = 2500.0f;

	/** 最大水平射程（cm），超出后裁剪。0 = 不限制 */
	float MaxThrowRange = 3000.0f;

	/** 默认投掷速度（cm/s），无有效目标或解算失败时回退 */
	float DefaultThrowSpeed = 1500.0f;

	/** 投射物碰撞球半径（cm），用于 PredictProjectilePath */
	float ProjectileRadius = 15.0f;

	/** 引信时间（秒），用于 PredictProjectilePath 最大模拟时间 */
	float FuseSeconds = 3.0f;

	/** 预测路径时忽略的 Actor（通常为投掷者自身） */
	AActor* ActorToIgnore = nullptr;

	/** 使用 DataAsset 填充参数（便捷方法） */
	static FGrenadeBallisticsInput FromDataAsset(const UWeaponGrenadeDataAsset* Data,
		const FVector& InSpawnLocation, const FVector2D& InAimDirection2D,
		const FVector& InTargetPoint, bool bInHasTarget, AActor* InActorToIgnore);
};

/**
 * 手雷弹道共享解算：输出结果
 */
struct FGrenadeBallisticsOutput
{
	/** 世界空间初速度向量（可直接传给 AGrenadeProjectile::SetInitialThrowVelocity） */
	FVector InitialVelocity = FVector::ZeroVector;

	/** 实际投掷初速大小（cm/s，clamp 后值） */
	float ActualThrowSpeed = 0.0f;

	/** 预测弹道路径点（PredictProjectilePath 的 PathData，世界坐标） */
	TArray<FVector> PathPoints;

	/** 预测爆炸点（世界坐标）：首次阻挡点，或 FuseSeconds 时间终点 */
	FVector ExplosionPoint = FVector::ZeroVector;

	/** 是否在路径中命中阻挡物（用于判断爆炸点来源） */
	bool bHitBlocking = false;

	/** 命中阻挡时的法线方向（仅在 bHitBlocking 时有效） */
	FVector HitNormal = FVector::ZeroVector;
};

/**
 * 手雷弹道共享解算函数
 *
 * 同时被 UGrenadeTrajectoryPreviewComponent（客户端预览）和
 * UGA_ThrowGrenade（服务器投掷）调用，确保：
 * - 速度解算公式完全一致
 * - PredictProjectilePath 参数完全一致
 * - 爆炸点判定逻辑完全一致
 */
namespace FGrenadeBallistics
{
	/**
	 * 执行完整弹道解算：速度计算 + 弹道预测
	 *
	 * @param Input  输入参数（SpawnLocation, AimDirection, TargetPoint, DataAsset 参数等）
	 * @param World  世界指针（用于 PredictProjectilePath 碰撞查询）
	 * @param Out    输出结果（InitialVelocity, PathPoints, ExplosionPoint 等）
	 * @return true 解算成功，false 失败（Out 中会使用回退速度填充）
	 */
	bool Calculate(const FGrenadeBallisticsInput& Input, UWorld* World, FGrenadeBallisticsOutput& Out);
}
