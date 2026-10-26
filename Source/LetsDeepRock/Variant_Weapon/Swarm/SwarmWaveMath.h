// Swarm 池化虫群：波次爆发数量计算(纯函数，便于单测)

#pragma once

#include "CoreMinimal.h"
#include "Online/WeaponSnapshot.h"
#include "Swarm/SwarmTypes.h"

/** 本次爆发应生成的数量 = Max(0, Min(剩余配额, 爆发大小, 上限 - 存活数)) */
LETSDEEPROCK_API int32 SwarmComputeBurst(int32 RemainingQuota, int32 Alive, int32 Cap, int32 BurstSize);

/**
 * 把存活虫按房间归并为 (房间, 原型) 计数。
 * 落在某房间盒内的归该房间;盒外的归中心最近的房间。无房间时返回空。输出按房间下标再按行名排序。
 */
LETSDEEPROCK_API TArray<FWeaponSwarmRoomCount> SwarmBucketByRoom(TConstArrayView<FSwarmBugView> Bugs, TConstArrayView<FBox2D> Rooms);
