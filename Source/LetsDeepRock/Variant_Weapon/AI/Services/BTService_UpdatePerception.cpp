// Copyright Epic Games, Inc. All Rights Reserved.

#include "BTService_UpdatePerception.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "AIController.h"
#include "EnemyAIController.h"
#include "AbilitySystemInterface.h"
#include "AbilitySystemComponent.h"
#include "WeaponGameplayTags.h"
#include "WeaponNPC.h"
#include "Attributes/WeaponHealthSet.h"
#include "Kismet/GameplayStatics.h"

UBTService_UpdatePerception::UBTService_UpdatePerception()
{
	NodeName = TEXT("Update Perception");

	// 默认 Tick 间隔 0.25s（不需要每帧更新）
	Interval = 0.25f;
	RandomDeviation = 0.05f;
}

void UBTService_UpdatePerception::TickNode(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds)
{
	Super::TickNode(OwnerComp, NodeMemory, DeltaSeconds);

	UBlackboardComponent* BB = OwnerComp.GetBlackboardComponent();
	if (!BB) return;

	AAIController* AIController = OwnerComp.GetAIOwner();
	if (!AIController) return;

	APawn* Pawn = AIController->GetPawn();
	if (!Pawn) return;

	// --- 感知数据更新 ---
	// 优先从 AI Perception 获取目标
	AEnemyAIController* EnemyAIC = Cast<AEnemyAIController>(AIController);
	AActor* Target = EnemyAIC ? EnemyAIC->GetPerceivedTarget() : nullptr;

	// 如果感知没有目标，直接获取玩家（敌人生成即追击，不需要等待感知）
	if (!Target || !IsValid(Target))
	{
		Target = UGameplayStatics::GetPlayerPawn(Pawn->GetWorld(), 0);
	}

	if (Target && IsValid(Target))
	{
		BB->SetValueAsObject(TargetActorKey.SelectedKeyName, Target);
		BB->SetValueAsVector(TargetLocationKey.SelectedKeyName, Target->GetActorLocation());
		BB->SetValueAsBool(HasTargetKey.SelectedKeyName, true);

		const float Distance = FVector::Dist(Pawn->GetActorLocation(), Target->GetActorLocation());
		BB->SetValueAsFloat(DistanceToTargetKey.SelectedKeyName, Distance);
	}
	else
	{
		BB->SetValueAsObject(TargetActorKey.SelectedKeyName, nullptr);
		BB->SetValueAsBool(HasTargetKey.SelectedKeyName, false);
		BB->SetValueAsFloat(DistanceToTargetKey.SelectedKeyName, 0.0f);
	}

	// --- GAS 状态更新 ---
	AWeaponNPC* NPC = Cast<AWeaponNPC>(Pawn);
	if (NPC && NPC->GetHealthSet())
	{
		const UWeaponHealthSet* HealthSet = NPC->GetHealthSet();
		const float MaxHealth = HealthSet->GetMaxHealth();
		const float HealthPct = (MaxHealth > 0.0f) ? (HealthSet->GetHealth() / MaxHealth) : 0.0f;
		BB->SetValueAsFloat(HealthPercentKey.SelectedKeyName, HealthPct);
	}
}
