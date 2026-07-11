// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTDecorator.h"
#include "BTDecorator_HealthBelow.generated.h"

/**
 * BT Decorator：判断自身血量百分比是否低于阈值
 * 从 Blackboard 读取 HealthPercent 浮点值
 */
UCLASS()
class UBTDecorator_HealthBelow : public UBTDecorator
{
	GENERATED_BODY()

public:
	UBTDecorator_HealthBelow();

	virtual FString GetStaticDescription() const override;

protected:
	virtual bool CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const override;

	/** Blackboard 键：血量百分比 */
	UPROPERTY(EditAnywhere, Category = "Condition")
	FBlackboardKeySelector HealthPercentKey;

	/** 血量阈值 (0.0~1.0)，低于此值时条件为 true */
	UPROPERTY(EditAnywhere, Category = "Condition", meta = (ClampMin = 0.0f, ClampMax = 1.0f))
	float Threshold = 0.2f;
};
