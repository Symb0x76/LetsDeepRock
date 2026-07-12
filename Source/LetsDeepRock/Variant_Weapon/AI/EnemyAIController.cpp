// Copyright Epic Games, Inc. All Rights Reserved.

#include "EnemyAIController.h"
#include "BehaviorTree/BehaviorTree.h"
#include "BehaviorTree/BehaviorTreeComponent.h"
#include "BehaviorTree/BlackboardComponent.h"
#include "Perception/AIPerceptionComponent.h"
#include "Perception/AISenseConfig_Sight.h"
#include "Perception/AISense_Sight.h"

AEnemyAIController::AEnemyAIController()
{
	// BehaviorTree 组件
	BehaviorTreeComponent = CreateDefaultSubobject<UBehaviorTreeComponent>(TEXT("BehaviorTreeComponent"));

	// Blackboard 组件
	BlackboardComponent = CreateDefaultSubobject<UBlackboardComponent>(TEXT("BlackboardComponent"));
	Blackboard = BlackboardComponent;

	// AI Perception 组件
	AIPerceptionComponent = CreateDefaultSubobject<UAIPerceptionComponent>(TEXT("AIPerception"));
	SetPerceptionComponent(*AIPerceptionComponent);

	// 视觉感知配置
	SightConfig = CreateDefaultSubobject<UAISenseConfig_Sight>(TEXT("SightConfig"));
	SightConfig->SightRadius = SightRadius;
	SightConfig->LoseSightRadius = LoseSightRadius;
	SightConfig->PeripheralVisionAngleDegrees = PeripheralVisionAngle;
	SightConfig->SetMaxAge(MaxAge);
	SightConfig->DetectionByAffiliation.bDetectEnemies = true;
	SightConfig->DetectionByAffiliation.bDetectNeutrals = true;
	SightConfig->DetectionByAffiliation.bDetectFriendlies = false;

	AIPerceptionComponent->ConfigureSense(*SightConfig);
	AIPerceptionComponent->SetDominantSense(UAISense_Sight::StaticClass());

	// 基础 AI 配置
	bStartAILogicOnPossess = true;
	bAttachToPawn = true;
}

void AEnemyAIController::BeginPlay()
{
	Super::BeginPlay();

	if (AIPerceptionComponent)
	{
		AIPerceptionComponent->OnTargetPerceptionUpdated.AddDynamic(this, &AEnemyAIController::OnTargetPerceptionUpdated);
	}
}

void AEnemyAIController::OnPossess(APawn* InPawn)
{
	Super::OnPossess(InPawn);

	// 更新感知参数
	if (SightConfig)
	{
		SightConfig->SightRadius = SightRadius;
		SightConfig->LoseSightRadius = LoseSightRadius;
		SightConfig->PeripheralVisionAngleDegrees = PeripheralVisionAngle;
		SightConfig->SetMaxAge(MaxAge);

		if (AIPerceptionComponent)
		{
			AIPerceptionComponent->RequestStimuliListenerUpdate();
		}
	}

	// 启动 BehaviorTree
	if (DefaultBehaviorTree)
	{
		if (DefaultBehaviorTree->BlackboardAsset)
		{
			BlackboardComponent->InitializeBlackboard(*DefaultBehaviorTree->BlackboardAsset);
		}

		// 设置 SelfActor 键
		BlackboardComponent->SetValueAsObject(TEXT("SelfActor"), InPawn);

		BehaviorTreeComponent->StartTree(*DefaultBehaviorTree);
	}
}

void AEnemyAIController::OnUnPossess()
{
	// 停止 BehaviorTree
	if (BehaviorTreeComponent)
	{
		BehaviorTreeComponent->StopTree();
	}

	Super::OnUnPossess();
}

void AEnemyAIController::OnTargetPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus)
{
	if (!Actor) return;

	if (Stimulus.WasSuccessfullySensed())
	{
		PerceivedTarget = Actor;
	}
	else
	{
		if (PerceivedTarget == Actor)
		{
			PerceivedTarget = nullptr;
		}
	}
}
