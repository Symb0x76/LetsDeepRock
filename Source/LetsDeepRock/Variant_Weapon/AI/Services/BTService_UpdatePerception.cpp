// Copyright Epic Games, Inc. All Rights Reserved.

#include "BTService_UpdatePerception.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "AIController.h"
#include "EnemyAIController.h"
#include "AbilitySystemInterface.h"
#include "AbilitySystemComponent.h"
#include "WeaponGameplayTags.h"
#include "Attributes/WeaponHealthSet.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Reinforcement/WeaponReinforcementRules.h"

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

	// 已阵亡的玩家 Pawn 仍留在场内:不再作为目标(倒地玩家仍可被锁定)
	if (Target && WeaponGetLifeState(Cast<APawn>(Target) ? Cast<APawn>(Target)->GetPlayerState() : nullptr) ==
		EWeaponPlayerLifeState::Dead)
	{
		Target = nullptr;
	}

	// 如果感知没有目标，取最近的非阵亡玩家（敌人生成即追击，不需要等待感知）
	if (!Target || !IsValid(Target))
	{
		float BestDistSquared = TNumericLimits<float>::Max();
		for (FConstPlayerControllerIterator It = Pawn->GetWorld()->GetPlayerControllerIterator(); It; ++It)
		{
			APawn* PlayerPawn = It->IsValid() ? It->Get()->GetPawn() : nullptr;
			if (!PlayerPawn || WeaponGetLifeState(PlayerPawn->GetPlayerState()) == EWeaponPlayerLifeState::Dead)
			{
				continue;
			}
			const float DistSquared = FVector::DistSquared(Pawn->GetActorLocation(), PlayerPawn->GetActorLocation());
			if (DistSquared < BestDistSquared)
			{
				BestDistSquared = DistSquared;
				Target = PlayerPawn;
			}
		}
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
	if (IAbilitySystemInterface* ASI = Cast<IAbilitySystemInterface>(Pawn))
	{
		if (UAbilitySystemComponent* ASC = ASI->GetAbilitySystemComponent())
		{
			if (const UWeaponHealthSet* HealthSet = ASC->GetSet<UWeaponHealthSet>())
			{
				const float MaxHealth = HealthSet->GetMaxHealth();
				const float HealthPct = (MaxHealth > 0.0f) ? (HealthSet->GetHealth() / MaxHealth) : 0.0f;
				BB->SetValueAsFloat(HealthPercentKey.SelectedKeyName, HealthPct);
			}
		}
	}
}
