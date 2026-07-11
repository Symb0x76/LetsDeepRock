// Copyright Epic Games, Inc. All Rights Reserved.

#include "BTTask_FindPatrolPoint.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "AIController.h"
#include "NavigationSystem.h"

UBTTask_FindPatrolPoint::UBTTask_FindPatrolPoint()
{
	NodeName = TEXT("Find Patrol Point");
}

EBTNodeResult::Type UBTTask_FindPatrolPoint::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AAIController* AIController = OwnerComp.GetAIOwner();
	if (!AIController) return EBTNodeResult::Failed;

	APawn* Pawn = AIController->GetPawn();
	if (!Pawn) return EBTNodeResult::Failed;

	const FVector Origin = Pawn->GetActorLocation();

	UNavigationSystemV1* NavSys = FNavigationSystem::GetCurrent<UNavigationSystemV1>(Pawn->GetWorld());
	if (!NavSys) return EBTNodeResult::Failed;

	FNavLocation RandomLocation;
	if (NavSys->GetRandomReachablePointInRadius(Origin, PatrolRadius, RandomLocation))
	{
		UBlackboardComponent* BB = OwnerComp.GetBlackboardComponent();
		if (BB)
		{
			BB->SetValueAsVector(PatrolLocationKey.SelectedKeyName, RandomLocation.Location);
			return EBTNodeResult::Succeeded;
		}
	}

	return EBTNodeResult::Failed;
}

FString UBTTask_FindPatrolPoint::GetStaticDescription() const
{
	return FString::Printf(TEXT("Patrol Radius: %.0f"), PatrolRadius);
}
