// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_FaceTarget.generated.h"

/**
 * BT Task：平滑旋转面向 Blackboard 中的目标 Actor
 * 旋转到面向后立即返回 Succeeded（非持续 Task）
 */
UCLASS()
class UBTTask_FaceTarget : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_FaceTarget();

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;

protected:
	/** Blackboard 键：目标 Actor */
	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector TargetActorKey;

	/** 旋转速度（度/秒） */
	UPROPERTY(EditAnywhere, Category = "FaceTarget", meta = (ClampMin = 1.0f))
	float RotationSpeed = 720.0f;

	/** 面向误差容忍角度（度），小于此角度视为面向完成 */
	UPROPERTY(EditAnywhere, Category = "FaceTarget", meta = (ClampMin = 1.0f))
	float AngleTolerance = 5.0f;
};
