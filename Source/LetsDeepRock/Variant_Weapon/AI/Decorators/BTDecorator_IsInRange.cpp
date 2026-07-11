// Copyright Epic Games, Inc. All Rights Reserved.

#include "BTDecorator_IsInRange.h"
#include "BehaviorTree/BlackboardComponent.h"

UBTDecorator_IsInRange::UBTDecorator_IsInRange()
{
	NodeName = TEXT("Is In Range");
}

bool UBTDecorator_IsInRange::CalculateRawConditionValue(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) const
{
	const UBlackboardComponent* BB = OwnerComp.GetBlackboardComponent();
	if (!BB) return false;

	const float Distance = BB->GetValueAsFloat(DistanceToTargetKey.SelectedKeyName);
	const bool bInRange = (Distance > 0.0f) && (Distance <= Range);

	return bInvert ? !bInRange : bInRange;
}

FString UBTDecorator_IsInRange::GetStaticDescription() const
{
	return FString::Printf(TEXT("Distance %s %.0f"), bInvert ? TEXT(">") : TEXT("<="), Range);
}
