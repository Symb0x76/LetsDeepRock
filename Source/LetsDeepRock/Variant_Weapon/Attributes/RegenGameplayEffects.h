// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "RegenGameplayEffects.generated.h"

/**
 *  C++ 构造的再生 GE（替代 AttributeRegenComponent 的 Tick 写值）。
 *
 *  语义：Infinite + Periodic，每个周期把 Rate 属性 × 周期长度加到资源上。
 *  写入走 GE 管线（ExecutedOn → PreAttributeChange clamp），不再绕过 aggregator：
 *  将来任何"再生倍率" GE（Aggregated modifier on Rate）会被自动纳入。
 *
 *  快照注意：CaptureAttributeValues 读 current、Restore 写 base，
 *  periodic pending 增量（≤ Rate×Period）会被 [0, Max] clamp 吸收。
 */
UCLASS()
class UGE_EnergyRegen : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UGE_EnergyRegen();
};

UCLASS()
class UGE_ShieldRegen : public UGameplayEffect
{
	GENERATED_BODY()

public:
	UGE_ShieldRegen();
};
