// Copyright Epic Games, Inc. All Rights Reserved.

#include "BTTask_FaceTarget.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "AIController.h"

UBTTask_FaceTarget::UBTTask_FaceTarget()
{
	NodeName = TEXT("Face Target");
	bNotifyTick = true;
}

EBTNodeResult::Type UBTTask_FaceTarget::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AAIController* AIController = OwnerComp.GetAIOwner();
	if (!AIController) return EBTNodeResult::Failed;

	APawn* Pawn = AIController->GetPawn();
	if (!Pawn) return EBTNodeResult::Failed;

	UBlackboardComponent* BB = OwnerComp.GetBlackboardComponent();
	if (!BB) return EBTNodeResult::Failed;

	AActor* Target = Cast<AActor>(BB->GetValueAsObject(TargetActorKey.SelectedKeyName));
	if (!Target || !IsValid(Target)) return EBTNodeResult::Failed;

	return EBTNodeResult::InProgress;
}

void UBTTask_FaceTarget::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
	AAIController* AIController = OwnerComp.GetAIOwner();
	if (!AIController)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	APawn* Pawn = AIController->GetPawn();
	if (!Pawn)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	UBlackboardComponent* BB = OwnerComp.GetBlackboardComponent();
	AActor* Target = BB ? Cast<AActor>(BB->GetValueAsObject(TargetActorKey.SelectedKeyName)) : nullptr;
	if (!Target || !IsValid(Target))
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Failed);
		return;
	}

	// 计算方向
	const FVector Direction = (Target->GetActorLocation() - Pawn->GetActorLocation()).GetSafeNormal2D();
	if (Direction.IsNearlyZero())
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
		return;
	}

	const FRotator TargetRotation = Direction.Rotation();
	const FRotator CurrentRotation = Pawn->GetActorRotation();

	// 检查是否已经面向目标
	const float AngleDiff = FMath::Abs(FRotator::NormalizeAxis(TargetRotation.Yaw - CurrentRotation.Yaw));
	if (AngleDiff <= AngleTolerance)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
		return;
	}

	// 平滑旋转
	const FRotator NewRotation = FMath::RInterpConstantTo(CurrentRotation, TargetRotation, DeltaSeconds, RotationSpeed);
	Pawn->SetActorRotation(FRotator(0.0f, NewRotation.Yaw, 0.0f));
}

EBTNodeResult::Type UBTTask_FaceTarget::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	return EBTNodeResult::Aborted;
}
