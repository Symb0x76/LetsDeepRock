// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "AttributeRegenComponent.generated.h"

class UAbilitySystemComponent;

/**
 * 属性自动恢复组件
 *
 * 处理护盾延迟再生和能量持续恢复。
 * 挂在玩家 Character 上（Tick 驱动），从 ASC 读取 Rate/Delay 属性控制恢复速度。
 * 仅服务端执行实际属性写入，写入使用 ASC->SetNumericAttributeBase()。
 *
 * 挂载方式：在 BP_WeaponCharacter 蓝图中手动添加本组件即可。
 */
UCLASS(ClassGroup = (GAS), meta = (BlueprintSpawnableComponent))
class UAttributeRegenComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UAttributeRegenComponent();

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

	/** 通知受到伤害，重置护盾再生延迟计时器 */
	void NotifyDamageTaken();

protected:
	virtual void BeginPlay() override;

private:
	/** 获取 Owner 的 ASC */
	UAbilitySystemComponent* GetASC() const;

	/** 护盾再生延迟计时器（受击后倒计时，归零后开始恢复） */
	float ShieldRegenTimer = 0.0f;

	/** 是否正在等待护盾恢复延迟 */
	bool bShieldRegenDelayed = true;
};
