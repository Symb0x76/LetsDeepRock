// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTService.h"
#include "BTService_UpdatePerception.generated.h"

/**
 * BT Service：每 Tick 从 AI Perception 和 GAS 更新 Blackboard 数据
 * 挂载在 BT Root 节点上，持续运行
 *
 * 更新以下 Blackboard 键：
 * - TargetActor (Object): 当前感知到的目标
 * - TargetLocation (Vector): 目标位置
 * - DistanceToTarget (Float): 到目标的距离
 * - bHasTarget (Bool): 是否有有效目标
 * - HealthPercent (Float): 自身血量百分比 (0.0~1.0)
 */
UCLASS()
class UBTService_UpdatePerception : public UBTService
{
	GENERATED_BODY()

public:
	UBTService_UpdatePerception();

protected:
	virtual void TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;

	/** Blackboard 键：目标 Actor */
	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector TargetActorKey;

	/** Blackboard 键：目标位置 */
	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector TargetLocationKey;

	/** Blackboard 键：到目标距离 */
	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector DistanceToTargetKey;

	/** Blackboard 键：是否有目标 */
	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector HasTargetKey;

	/** Blackboard 键：血量百分比 */
	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector HealthPercentKey;
};
