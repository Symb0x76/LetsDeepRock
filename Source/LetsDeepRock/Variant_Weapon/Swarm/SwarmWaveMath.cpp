#include "Swarm/SwarmWaveMath.h"

int32 SwarmComputeBurst(const int32 RemainingQuota, const int32 Alive, const int32 Cap, const int32 BurstSize)
{
	return FMath::Max(0, FMath::Min3(RemainingQuota, BurstSize, Cap - Alive));
}
