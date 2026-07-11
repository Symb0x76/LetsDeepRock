// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "BehaviorTree/BTTaskNode.h"
#include "Abilities/GameplayAbility.h"
#include "BTTask_Attack.generated.h"

/**
 * BT Task：触发 GAS GameplayAbility 进行攻击
 * 激活能力后等待冷却时间，然后返回 Succeeded
 */
UCLASS()
class UBTTask_Attack : public UBTTaskNode
{
	GENERATED_BODY()

public:
	UBTTask_Attack();

	virtual EBTNodeResult::Type ExecuteTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual void TickTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory, float DeltaSeconds) override;
	virtual EBTNodeResult::Type AbortTask(UBehaviorTreeComponent& OwnerComp, uint8* NodeMemory) override;
	virtual FString GetStaticDescription() const override;

protected:
	/** 要触发的 GameplayAbility 类 */
	UPROPERTY(EditAnywhere, Category = "Attack")
	TSubclassOf<UGameplayAbility> AbilityClass;

	/** 攻击后的冷却时间（秒） */
	UPROPERTY(EditAnywhere, Category = "Attack", meta = (ClampMin = 0.0f))
	float AttackCooldown = 1.5f;

private:
	/** Task 内部内存结构 */
	struct FBTTask_AttackMemory
	{
		float CooldownRemaining;
	};

	virtual uint16 GetInstanceMemorySize() const override { return sizeof(FBTTask_AttackMemory); }
};
