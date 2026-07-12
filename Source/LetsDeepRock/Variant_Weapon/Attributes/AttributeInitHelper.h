// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FCharacterAttributeRow;
struct FEnemyConfigRow;
class UAbilitySystemComponent;

/**
 * 属性初始化工具
 * 从 DataTable 行结构读取数值，通过 Init 方法设置 ASC 上所有 AttributeSet 的基础值。
 */
class FAttributeInitHelper
{
public:
	/** 使用角色属性行初始化玩家 ASC 的所有属性（全部 5 个 Set） */
	static void InitializePlayerAttributes(UAbilitySystemComponent* ASC, const FCharacterAttributeRow& Row);

	/** 使用敌人配置行初始化 NPC ASC 的属性（HealthSet + ResistanceSet + MovementSet） */
	static void InitializeNPCAttributes(UAbilitySystemComponent* ASC, const FEnemyConfigRow& Row);
};
