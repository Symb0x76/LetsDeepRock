// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "NativeGameplayTags.h"
#include "EnemyAITypes.generated.h"

// AI 状态 Tag（用于 StateTree 转移条件和调试显示）
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_AI_State_Patrol);
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_AI_State_Chase);
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_AI_State_Attack);
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_AI_State_Flee);

/** 敌人 AI 行为状态枚举（用于调试和事件通知） */
UENUM(BlueprintType)
enum class EEnemyAIState : uint8
{
	Idle        UMETA(DisplayName = "Idle"),
	Patrol      UMETA(DisplayName = "Patrol"),
	Chase       UMETA(DisplayName = "Chase"),
	Attack      UMETA(DisplayName = "Attack"),
	Flee        UMETA(DisplayName = "Flee"),
	Dead        UMETA(DisplayName = "Dead")
};
