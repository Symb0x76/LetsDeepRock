// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTDecorator.h"
#include "BTDecorator_IsInRange.generated.h"

/**
 * BT Decorator：判断到目标的距离是否在指定范围内
 * 从 Blackboard 读取 DistanceToTarget 浮点值进行判断
 */
UCLASS()
class UBTDecorator_IsInRange : public UBTDecorator
{
	GENERATED_BODY()

public:
	UBTDecorator_IsInRange();

	virtual FString GetStaticDescription() const override;

protected:
	virtual bool CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const override;

	/** Blackboard 键：到目标的距离 */
	UPROPERTY(EditAnywhere, Category = "Condition")
	FBlackboardKeySelector DistanceToTargetKey;

	/** 判定距离阈值 */
	UPROPERTY(EditAnywhere, Category = "Condition", meta = (ClampMin = 0.0f))
	float Range = 200.0f;

	/** 取反：true 时判断"不在范围内" */
	UPROPERTY(EditAnywhere, Category = "Condition")
	bool bInvert = false;
};
