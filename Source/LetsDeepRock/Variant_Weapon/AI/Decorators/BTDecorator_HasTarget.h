// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTDecorator.h"
#include "BTDecorator_HasTarget.generated.h"

/**
 * BT Decorator：判断是否有有效的感知目标
 * 从 Blackboard 读取 bHasTarget 布尔值
 */
UCLASS()
class UBTDecorator_HasTarget : public UBTDecorator
{
	GENERATED_BODY()

public:
	UBTDecorator_HasTarget();

protected:
	virtual bool CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const override;

	/** Blackboard 键：是否有目标 */
	UPROPERTY(EditAnywhere, Category = "Condition")
	FBlackboardKeySelector HasTargetKey;

	/** 取反：true 时判断"没有目标" */
	UPROPERTY(EditAnywhere, Category = "Condition")
	bool bInvert = false;
};
