// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffectExecutionCalculation.h"
#include "DamageExecCalculation.generated.h"

/**
 * 伤害公式执行计算（ExecCalc）
 *
 * 完整伤害流程：
 * ① 基础伤害（SetByCaller Data.Damage）
 * ② 全局伤害加成（DamageBonus）
 * ③ 伤害类型克制修正（TODO: DataTable 查询，暂硬编码 1.0）
 * ④ 暴击判定（CritChance → CritDamage 倍率）
 * ⑤ 弱点加成（TODO: 后续扩展，暂不实现）
 * ⑥ 护甲减伤：EffectiveArmor = Armor × (1 - ArmorPenetration)，DR = EffArmor / (EffArmor + 200)
 * ⑦ 抗性减免（根据伤害类型 Tag 选择对应抗性）
 * ⑧ 输出到 Damage Meta Attribute → WeaponHealthSet::PostGameplayEffectExecute 接收
 *
 * True 伤害跳过护甲和抗性计算。
 */
UCLASS()
class UDamageExecCalculation : public UGameplayEffectExecutionCalculation
{
	GENERATED_BODY()

public:

	UDamageExecCalculation();

	virtual void Execute_Implementation(const FGameplayEffectCustomExecutionParameters& ExecutionParams,
		FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const override;
};
