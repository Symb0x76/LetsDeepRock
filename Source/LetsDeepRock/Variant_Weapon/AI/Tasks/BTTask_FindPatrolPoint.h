// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "BTTask_FindPatrolPoint.generated.h"

/**
 * BT Task：在 NavMesh 上随机选取一个巡逻点
 * 写入 Blackboard 的 PatrolLocation 键
 */
UCLASS()
class UBTTask_FindPatrolPoint : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_FindPatrolPoint();

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual FString GetStaticDescription() const override;

protected:
	/** Blackboard 键：输出巡逻位置 */
	UPROPERTY(EditAnywhere, Category = "Blackboard")
	FBlackboardKeySelector PatrolLocationKey;

	/** 巡逻半径 */
	UPROPERTY(EditAnywhere, Category = "Patrol", meta = (ClampMin = 100.0f))
	float PatrolRadius = 800.0f;
};
