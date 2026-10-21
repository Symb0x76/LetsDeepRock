#include "Swarm/SwarmSteering.h"
#include "Swarm/SwarmTypes.h"

namespace SwarmSteeringConstants
{
	inline constexpr float SeparationWeight = 1.5f;
	inline constexpr float SpawnRingSpacing = 50.f;
	inline constexpr int32 SpawnRingSamples = 8;
}

FSwarmSteeringResult SwarmComputeSteeringStep(const FSwarmSteeringInput& In, TFunctionRef<bool(const FVector2D&)> IsBlocked)
{
	FVector2D Velocity = FVector2D::ZeroVector;
	if (In.bHasTarget)
	{
		Velocity += (In.TargetPosition - In.Position).GetSafeNormal() * In.MaxSpeed;
	}

	for (const FVector2D& Neighbor : In.Neighbors)
	{
		const FVector2D Away = In.Position - Neighbor;
		const float Distance = Away.Size();
		// 重合时没有方向可言，跳过以避免 NaN
		if (Distance < KINDA_SMALL_NUMBER || Distance >= SwarmConstants::SeparationRadius)
		{
			continue;
		}
		Velocity += (Away / Distance) * (1.f - Distance / SwarmConstants::SeparationRadius)
			* In.MaxSpeed * SwarmSteeringConstants::SeparationWeight;
	}
	Velocity = Velocity.GetClampedToMaxSize(In.MaxSpeed);

	FSwarmSteeringResult Result;
	FVector2D Candidate = In.Position + Velocity * In.DeltaSeconds;

	for (const FSwarmObstacle& Obstacle : In.Obstacles)
	{
		const float MinDistance = Obstacle.Radius + In.SelfRadius;
		const FVector2D Offset = Candidate - Obstacle.Position;
		if (Offset.SizeSquared() < FMath::Square(MinDistance))
		{
			FVector2D Direction = Offset.GetSafeNormal();
			if (Direction.IsZero())
			{
				Direction = (In.Position - Obstacle.Position).GetSafeNormal();
			}
			if (Direction.IsZero())
			{
				Direction = FVector2D(1.0, 0.0);
			}
			Candidate = Obstacle.Position + Direction * MinDistance;
			Result.bTouchingObstacle = true;
		}
	}

	if (IsBlocked(Candidate))
	{
		const FVector2D XOnly(Candidate.X, In.Position.Y);
		const FVector2D YOnly(In.Position.X, Candidate.Y);
		if (!IsBlocked(XOnly))
		{
			Candidate = XOnly;
		}
		else if (!IsBlocked(YOnly))
		{
			Candidate = YOnly;
		}
		else
		{
			Candidate = In.Position;
			Result.bBlockedByTerrain = true;
		}
	}

	Result.NewPosition = Candidate;
	return Result;
}

bool SwarmFindFreeSpawnPoint(const FVector2D& Desired, float SearchRadius, TFunctionRef<bool(const FVector2D&)> IsBlocked, FVector2D& OutPoint)
{
	if (!IsBlocked(Desired))
	{
		OutPoint = Desired;
		return true;
	}

	for (float Ring = SwarmSteeringConstants::SpawnRingSpacing; Ring <= SearchRadius; Ring += SwarmSteeringConstants::SpawnRingSpacing)
	{
		for (int32 Sample = 0; Sample < SwarmSteeringConstants::SpawnRingSamples; ++Sample)
		{
			const float Angle = UE_TWO_PI * Sample / SwarmSteeringConstants::SpawnRingSamples;
			const FVector2D Point = Desired + FVector2D(FMath::Cos(Angle), FMath::Sin(Angle)) * Ring;
			if (!IsBlocked(Point))
			{
				OutPoint = Point;
				return true;
			}
		}
	}
	return false;
}
