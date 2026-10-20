// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayEffect.h"
#include "Data/DataTableRows.h"
#include "StatusEffectComponent.generated.h"

class UGameplayEffect;
class UAbilitySystemComponent;

struct FAppliedStatusEffect
{
	/** 配置行快照（伤害/类型判定/易伤倍率数据源） */
	FStatusEffectConfigRow Config;

	/** 命中携带的武器伤害 GE（周期伤害走完整伤害公式） */
	TSubclassOf<UGameplayEffect> DamageEffectClass;

	/** 承载时长/叠层/衰减的活跃 GE 句柄（状态的唯一权威存储） */
	FActiveGameplayEffectHandle Handle;

	/** 周期伤害定时器（5.8 无公开周期执行订阅点，伤害留在组件侧驱动） */
	FTimerHandle TickTimer;

	/** HitsPerStack 应用策略计数 */
	int32 PendingHits = 0;
};

/** 快照用的单条状态(Host Migration):GE 的叠层/剩余时长 + 组件侧的应用策略状态。 */
struct FStatusEffectSnapshotEntry
{
	FName StatusEffectId;
	int32 StackCount = 0;
	float RemainingDuration = 0.0f;
	int32 PendingHits = 0;
	TSubclassOf<UGameplayEffect> DamageEffectClass;
};

/**
 *  仅服务端运行的 NPC 状态容器；同一 Id 的命中跨来源共享。
 *
 *  职责划分（GE 化后）：时长/叠层/衰减由 UStatusEffectGameplayEffect（GAS 原生）承担，
 *  本组件只保留：ApplyChance/HitsPerStack 应用策略、周期伤害发射器（走武器伤害 GE
 *  与 DamageExecCalculation 完整公式）、易伤倍率查询、恢复屏障暂停。
 */
UCLASS(ClassGroup = (Weapon), meta = (BlueprintSpawnableComponent))
class LETSDEEPROCK_API UStatusEffectComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UStatusEffectComponent();

	bool TryApply(FName StatusEffectId, TSubclassOf<UGameplayEffect> DamageEffectClass);
	float GetVulnerableMultiplier() const;
	void SetGameplayRecoveryBlocked(bool bBlocked);
	void ClearAllEffects();

	/** 快照:导出仍有效的状态(已掉光的条目跳过) */
	void CaptureSnapshotState(TArray<FStatusEffectSnapshotEntry>& OutEntries) const;

	/**
	 *  恢复:按快照的叠层与剩余时长直接施加 GE 并重建周期伤害定时器。
	 *  只在恢复屏障内由 Snapshot Coordinator 调用,因此不走 TryApply 的屏障/概率/HitsPerStack 判定;
	 *  定时器创建后若屏障仍关闭则立即暂停,随屏障释放续跑。
	 */
	bool RestoreSnapshotEntry(const FStatusEffectSnapshotEntry& Entry);

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	TMap<FName, FAppliedStatusEffect> ActiveEffects;
	bool bGameplayRecoveryBlocked = false;

	UAbilitySystemComponent* GetASC() const;
	void TickDamage(FName StatusEffectId);
	void RemoveEffect(FName StatusEffectId);
	static bool IsEffectType(const FName StatusEffectId, const FStatusEffectConfigRow& Config, const TCHAR* TypeName);
};
