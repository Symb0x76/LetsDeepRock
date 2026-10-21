// Swarm 池化虫群：可受虫群伤害的对象接口

#pragma once

#include "CoreMinimal.h"
#include "UObject/Interface.h"
#include "Swarm/SwarmTypes.h"
#include "SwarmDamageable.generated.h"

UINTERFACE(MinimalAPI, meta = (CannotImplementInterfaceInBlueprint))
class USwarmDamageable : public UInterface
{
	GENERATED_BODY()
};

/** 虫群伤害接收者；纯 C++ 虚函数，不走蓝图事件 */
class LETSDEEPROCK_API ISwarmDamageable
{
	GENERATED_BODY()

public:
	virtual void ReceiveSwarmDamage(const FSwarmDamageEvent& Event) = 0;
};
