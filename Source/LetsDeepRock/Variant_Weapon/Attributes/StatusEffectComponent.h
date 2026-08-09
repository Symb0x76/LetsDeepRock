// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "Data/DataTableRows.h"
#include "StatusEffectComponent.generated.h"

class UGameplayEffect;

struct FActiveStatusEffect
{
	FStatusEffectConfigRow Config;
	TSubclassOf<UGameplayEffect> DamageEffectClass;
	int32 Stacks = 0;
	int32 PendingHits = 0;
	FTimerHandle DurationTimer;
	FTimerHandle DecayTimer;
	FTimerHandle TickTimer;
};

/** 仅服务端运行的 NPC 状态容器；同一 Id 的命中跨来源共享。 */
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

protected:
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

private:
	TMap<FName, FActiveStatusEffect> ActiveEffects;
	bool bGameplayRecoveryBlocked = false;

	void BeginDecay(FName StatusEffectId);
	void DecayStack(FName StatusEffectId);
	void TickDamage(FName StatusEffectId);
	void RemoveEffect(FName StatusEffectId);
	static bool IsEffectType(const FName StatusEffectId, const FStatusEffectConfigRow& Config, const TCHAR* TypeName);
};
