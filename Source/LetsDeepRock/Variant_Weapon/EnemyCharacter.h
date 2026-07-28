// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NPCBase.h"
#include "EnemyCharacter.generated.h"

/**
 *  敌人角色
 *
 *  在 ANPCBase 的 GAS 基础设施之上叠加敌人专属行为：
 *  - AEnemyAIController 自动 Possess
 *  - DT_EnemyConfig DataTable 驱动属性初始化
 */
UCLASS()
class AEnemyCharacter : public ANPCBase
{
	GENERATED_BODY()

public:

	AEnemyCharacter();

	FName GetEnemyConfigRowName() const { return EnemyConfigRowName; }

protected:

	virtual void BeginPlay() override;

	/** 配置表行名（对应 DT_EnemyConfig 中的 RowName） */
	UPROPERTY(EditAnywhere, Category = "DataTable")
	FName EnemyConfigRowName = NAME_None;
};
