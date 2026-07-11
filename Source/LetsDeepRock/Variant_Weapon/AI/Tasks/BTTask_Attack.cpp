// Copyright Epic Games, Inc. All Rights Reserved.

#include "BTTask_Attack.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "AIController.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"

UBTTask_Attack::UBTTask_Attack()
{
	NodeName = TEXT("Attack");
	bNotifyTick = true;  // 需要 TickTask
}

EBTNodeResult::Type UBTTask_Attack::ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	AAIController* AIController = OwnerComp.GetAIOwner();
	if (!AIController || !AbilityClass) return EBTNodeResult::Failed;

	APawn* Pawn = AIController->GetPawn();
	if (!Pawn) return EBTNodeResult::Failed;

	// 获取 ASC
	IAbilitySystemInterface* ASCInterface = Cast<IAbilitySystemInterface>(Pawn);
	if (!ASCInterface) return EBTNodeResult::Failed;

	UAbilitySystemComponent* ASC = ASCInterface->GetAbilitySystemComponent();
	if (!ASC) return EBTNodeResult::Failed;

	// 尝试激活能力
	bool bActivated = ASC->TryActivateAbilityByClass(AbilityClass, true);
	if (!bActivated) return EBTNodeResult::Failed;

	// 进入冷却等待
	FBTTask_AttackMemory* Memory = reinterpret_cast<FBTTask_AttackMemory*>(NodeMemory);
	Memory->CooldownRemaining = AttackCooldown;

	return EBTNodeResult::InProgress;
}

void UBTTask_Attack::TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
	FBTTask_AttackMemory* Memory = reinterpret_cast<FBTTask_AttackMemory*>(NodeMemory);
	Memory->CooldownRemaining -= DeltaSeconds;

	if (Memory->CooldownRemaining <= 0.0f)
	{
		FinishLatentTask(OwnerComp, EBTNodeResult::Succeeded);
	}
}

EBTNodeResult::Type UBTTask_Attack::AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory)
{
	return EBTNodeResult::Aborted;
}

FString UBTTask_Attack::GetStaticDescription() const
{
	FString AbilityName = AbilityClass ? AbilityClass->GetName() : TEXT("None");
	return FString::Printf(TEXT("Ability: %s, Cooldown: %.1fs"), *AbilityName, AttackCooldown);
}
