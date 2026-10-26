// Swarm 池化虫群：追击 + 分离转向（纯逻辑，地形阻挡通过谓词注入）

#pragma once

#include "CoreMinimal.h"
#include "Templates/Function.h"

/** 圆形障碍（玩家、精英等，虫会被推出其表面） */
struct FSwarmObstacle
{
	FVector2D Position = FVector2D::ZeroVector;
	float Radius = 0.f;
};

struct FSwarmSteeringInput
{
	FVector2D Position = FVector2D::ZeroVector;
	FVector2D TargetPosition = FVector2D::ZeroVector;
	bool bHasTarget = false;
	float MaxSpeed = 0.f;
	float DeltaSeconds = 0.f;
	float SelfRadius = 0.f;
	TConstArrayView<FVector2D> Neighbors;
	TConstArrayView<FSwarmObstacle> Obstacles;
};

struct FSwarmSteeringResult
{
	FVector2D NewPosition = FVector2D::ZeroVector;
	bool bBlockedByTerrain = false;
	bool bTouchingObstacle = false;
};

/** 单步转向：seek + 分离，障碍投影，地形阻挡时沿轴滑动 */
FSwarmSteeringResult SwarmComputeSteeringStep(const FSwarmSteeringInput& In, TFunctionRef<bool(const FVector2D&)> IsBlocked);

/** 以 Desired 为中心按 50uu 间隔的环、每环 8 个采样寻找未被阻挡的点；Desired 本身可用则直接返回 */
bool SwarmFindFreeSpawnPoint(const FVector2D& Desired, float SearchRadius, TFunctionRef<bool(const FVector2D&)> IsBlocked, FVector2D& OutPoint);
