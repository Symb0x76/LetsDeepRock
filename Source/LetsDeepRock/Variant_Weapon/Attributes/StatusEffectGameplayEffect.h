// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameplayEffect.h"
#include "Data/DataTableRows.h"
#include "StatusEffectGameplayEffect.generated.h"

/**
 *  DataTable 驱动的状态效果 GE（每个配置行一个瞬态实例，按行缓存）。
 *
 *  承担原 StatusEffectComponent 手写的三件事：
 *  - Duration：到期即走 StackExpirationPolicy
 *  - 叠层：AggregateByTarget + StackLimitCount（跨来源共享同一行实例）
 *  - 衰减：RemoveSingleStackAndRefreshDuration —— 到期掉一层并刷新时长，
 *    全部掉光后 GAS 自动移除活跃效果
 *  - 授予 StatusTag（活跃期间可被 ASC tag 查询/AI/能力阻断使用）
 *
 *  不承担（留在组件，见 StatusEffectComponent 注释）：
 *  - ApplyChance/HitsPerStack 应用策略
 *  - 周期伤害（5.8 无公开的周期执行订阅点；伤害需走武器伤害 GE 与完整公式）
 */
UCLASS()
class UStatusEffectGameplayEffect : public UGameplayEffect
{
	GENERATED_BODY()

public:
	/** 按行取/建瞬态 GE 实例（行内容变更需重启进程，DataTable 热更新不感知） */
	static UStatusEffectGameplayEffect* GetOrCreate(const FName RowId, const FStatusEffectConfigRow& Config);

private:
	static TMap<FName, TObjectPtr<UStatusEffectGameplayEffect>> RowCache;
};
