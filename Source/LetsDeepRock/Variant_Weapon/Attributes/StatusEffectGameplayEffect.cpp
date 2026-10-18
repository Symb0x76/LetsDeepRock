// Copyright Epic Games, Inc. All Rights Reserved.

#include "StatusEffectGameplayEffect.h"

#include "UObject/Package.h"
#include "UObject/UObjectGlobals.h"

TMap<FName, TObjectPtr<UStatusEffectGameplayEffect>> UStatusEffectGameplayEffect::RowCache;

UStatusEffectGameplayEffect* UStatusEffectGameplayEffect::GetOrCreate(const FName RowId, const FStatusEffectConfigRow& Config)
{
	if (const TObjectPtr<UStatusEffectGameplayEffect>* Existing = RowCache.Find(RowId))
	{
		if (IsValid(*Existing))
		{
			return *Existing;
		}
	}

	UStatusEffectGameplayEffect* Effect = NewObject<UStatusEffectGameplayEffect>(
		GetTransientPackage(), NAME_None, RF_Standalone | RF_Transient);
	// AddToRoot：静态缓存里的 TObjectPtr 不构成 GC 根，需显式钉住（数量 = 配置行数，有界）。
	Effect->AddToRoot();

	PRAGMA_DISABLE_DEPRECATION_WARNINGS
	// 时长按 Spec 传入（SetDuration）；这里只声明"有时长"。
	Effect->DurationPolicy = EGameplayEffectDurationType::HasDuration;
	// 同一行 Id 跨来源聚合到同一活跃效果上（与旧组件"同一 Id 命中共享"一致）。
	Effect->StackingType = EGameplayEffectStackingType::AggregateByTarget;
	Effect->StackLimitCount = FMath::Max(Config.MaxStacks, 1);
	// 叠层刷新时长 == 旧 DurationTimer 每次叠层被 ClearTimer 重设。
	Effect->StackDurationRefreshPolicy = EGameplayEffectStackingDurationPolicy::RefreshOnSuccessfulApplication;
	// 到期掉一层并刷新时长 == 旧 DecayTimer 的 1 层/次衰减；掉光后 GAS 自动移除。
	Effect->StackExpirationPolicy = EGameplayEffectStackingExpirationPolicy::RemoveSingleStackAndRefreshDuration;
	PRAGMA_ENABLE_DEPRECATION_WARNINGS

	if (Config.StatusTag.IsValid())
	{
		Effect->InheritableOwnedTagsContainer.AddTag(Config.StatusTag);
	}

	RowCache.Add(RowId, Effect);
	return Effect;
}
