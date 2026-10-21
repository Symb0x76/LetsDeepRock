// Swarm 池化虫群：武器伤害路由（在 GAS 路径之前拦截虫群目标）

#pragma once

#include "CoreMinimal.h"
#include "Swarm/SwarmTypes.h"

class AActor;

/** 伤害来源是否允许伤害虫群。精英/NPC（ANPCBase）为虫群友军，返回 false；玩家、无人机与 nullptr 返回 true */
LETSDEEPROCK_API bool SwarmIsDamageInstigatorAllowed(const AActor* Instigator);

/**
 * 目标实现 ISwarmDamageable 时返回 true：来源被允许则转发事件，否则吞掉；
 * 目标不是虫群则返回 false，调用方继续走 GAS 路径。
 */
LETSDEEPROCK_API bool SwarmTryRouteDamage(AActor* Target, const FSwarmDamageEvent& Event);
