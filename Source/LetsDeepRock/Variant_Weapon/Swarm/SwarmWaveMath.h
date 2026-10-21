// Swarm 池化虫群：波次爆发数量计算(纯函数，便于单测)

#pragma once

#include "CoreMinimal.h"

/** 本次爆发应生成的数量 = Max(0, Min(剩余配额, 爆发大小, 上限 - 存活数)) */
LETSDEEPROCK_API int32 SwarmComputeBurst(int32 RemainingQuota, int32 Alive, int32 Cap, int32 BurstSize);
