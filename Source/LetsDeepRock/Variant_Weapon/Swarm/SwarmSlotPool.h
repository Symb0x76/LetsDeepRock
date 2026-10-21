// Swarm 池化虫群：固定容量槽位池（空闲链表 + 活跃位图）

#pragma once

#include "CoreMinimal.h"

/** 固定容量槽位池：Acquire/Release O(1)，槽位永不销毁，仅回收复用 */
class FSwarmSlotPool
{
public:
	explicit FSwarmSlotPool(int32 InCapacity);

	/** 取一个空闲槽位；池满返回 INDEX_NONE */
	int32 Acquire();

	/** 归还槽位；未激活或越界的槽位为空操作 */
	void Release(int32 Slot);

	bool IsActive(int32 Slot) const;
	int32 NumActive() const { return ActiveCount; }
	int32 Capacity() const { return CapacityValue; }

	/** 按槽位升序遍历活跃槽位，Fn(int32 Slot) */
	template<typename F>
	void ForEachActive(F&& Fn) const
	{
		for (TConstSetBitIterator<> It(ActiveFlags); It; ++It)
		{
			Fn(It.GetIndex());
		}
	}

private:
	int32 CapacityValue = 0;
	int32 ActiveCount = 0;
	TArray<int32> FreeList;
	TBitArray<> ActiveFlags;
};
