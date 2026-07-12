// Copyright Epic Games, Inc. All Rights Reserved.

#include "BTDecorator_HealthBelow.h"
#include "BehaviorTree/BlackboardComponent.h"

UBTDecorator_HealthBelow::UBTDecorator_HealthBelow()
{
	NodeName = TEXT("Health Below Threshold");
}

bool UBTDecorator_HealthBelow::CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const
{
	const UBlackboardComponent* BB = OwnerComp.GetBlackboardComponent();
	if (!BB) return false;

	const float HealthPercent = BB->GetValueAsFloat(HealthPercentKey.SelectedKeyName);
	return HealthPercent < Threshold;
}

FString UBTDecorator_HealthBelow::GetStaticDescription() const
{
	return FString::Printf(TEXT("Health < %.0f%%"), Threshold * 100.0f);
}
