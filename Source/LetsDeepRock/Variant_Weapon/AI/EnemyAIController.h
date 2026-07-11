// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "AIController.h"
#include "Perception/AIPerceptionTypes.h"
#include "EnemyAIController.generated.h"

class UBehaviorTreeComponent;
class UBlackboardComponent;
class UAIPerceptionComponent;
class UAISenseConfig_Sight;
class UBehaviorTree;
class UBlackboardData;

/**
 * 敌人 AI 控制器
 * 使用 BehaviorTree 驱动行为逻辑，AI Perception 感知玩家
 */
UCLASS()
class AEnemyAIController : public AAIController
{
	GENERATED_BODY()

public:
	AEnemyAIController();

	/** 获取当前感知到的目标 */
	UFUNCTION(BlueprintCallable, Category = "AI")
	AActor* GetPerceivedTarget() const { return PerceivedTarget; }

protected:
	virtual void BeginPlay() override;
	virtual void OnPossess(APawn* InPawn) override;
	virtual void OnUnPossess() override;

	/** 感知目标更新回调 */
	UFUNCTION()
	void OnTargetPerceptionUpdated(AActor* Actor, FAIStimulus Stimulus);

	/** BehaviorTree 组件 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "AI|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UBehaviorTreeComponent> BehaviorTreeComponent;

	/** Blackboard 组件 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "AI|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UBlackboardComponent> BlackboardComponent;

	/** AI 感知组件 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "AI|Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UAIPerceptionComponent> AIPerceptionComponent;

	/** 视觉感知配置 */
	UPROPERTY()
	TObjectPtr<UAISenseConfig_Sight> SightConfig;

	/** 默认 BehaviorTree 资产（在蓝图子类中配置） */
	UPROPERTY(EditAnywhere, Category = "AI|BehaviorTree")
	TObjectPtr<UBehaviorTree> DefaultBehaviorTree;

	/** 感知半径（视觉） */
	UPROPERTY(EditAnywhere, Category = "AI|Perception")
	float SightRadius = 1500.0f;

	/** 丢失目标的视觉距离 */
	UPROPERTY(EditAnywhere, Category = "AI|Perception")
	float LoseSightRadius = 2000.0f;

	/** 视野角度（单侧，180 = 全方位感知） */
	UPROPERTY(EditAnywhere, Category = "AI|Perception")
	float PeripheralVisionAngle = 180.0f;

	/** 丢失目标后的最大记忆时间（秒） */
	UPROPERTY(EditAnywhere, Category = "AI|Perception")
	float MaxAge = 5.0f;

private:
	/** 当前感知到的目标（最高优先级） */
	UPROPERTY()
	TObjectPtr<AActor> PerceivedTarget;
};
