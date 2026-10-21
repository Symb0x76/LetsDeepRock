#include "Swarm/SwarmSlotPool.h"

FSwarmSlotPool::FSwarmSlotPool(int32 InCapacity)
	: CapacityValue(FMath::Max(InCapacity, 0))
	, ActiveFlags(false, FMath::Max(InCapacity, 0))
{
	// 逆序入栈，使首次 Acquire 依次得到 0,1,2...
	FreeList.Reserve(CapacityValue);
	for (int32 Slot = CapacityValue - 1; Slot >= 0; --Slot)
	{
		FreeList.Add(Slot);
	}
}

int32 FSwarmSlotPool::Acquire()
{
	if (FreeList.IsEmpty())
	{
		return INDEX_NONE;
	}
	const int32 Slot = FreeList.Pop(EAllowShrinking::No);
	ActiveFlags[Slot] = true;
	++ActiveCount;
	return Slot;
}

void FSwarmSlotPool::Release(int32 Slot)
{
	if (!IsActive(Slot))
	{
		return;
	}
	ActiveFlags[Slot] = false;
	--ActiveCount;
	FreeList.Add(Slot);
}

bool FSwarmSlotPool::IsActive(int32 Slot) const
{
	return Slot >= 0 && Slot < CapacityValue && ActiveFlags[Slot];
}
