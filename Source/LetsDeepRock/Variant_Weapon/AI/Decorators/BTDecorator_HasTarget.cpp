// Copyright Epic Games, Inc. All Rights Reserved.

#include "BTDecorator_HasTarget.h"
#include "BehaviorTree/BlackboardComponent.h"

UBTDecorator_HasTarget::UBTDecorator_HasTarget()
{
	NodeName = TEXT("Has Target");
}

bool UBTDecorator_HasTarget::CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const
{
	const UBlackboardComponent* BB = OwnerComp.GetBlackboardComponent();
	if (!BB) return false;

	const bool bHasTarget = BB->GetValueAsBool(HasTargetKey.SelectedKeyName);
	return bInvert ? !bHasTarget : bHasTarget;
}
