// Swarm 池化虫群：二维均匀网格空间哈希（邻居查询）

#pragma once

#include "CoreMinimal.h"

/** 二维均匀网格空间哈希，每帧 Reset 后重新 Insert，Query 按精确距离过滤 */
class FSwarmSpatialHash
{
public:
	explicit FSwarmSpatialHash(float InCellSize);

	void Reset();
	void Insert(int32 Id, const FVector2D& Position);

	/** 收集 Center 周围 Radius 内（含边界）的所有 Id，顺序不保证 */
	void Query(const FVector2D& Center, float Radius, TArray<int32>& OutIds) const;

private:
	struct FEntry
	{
		int32 Id = INDEX_NONE;
		FVector2D Position = FVector2D::ZeroVector;
	};

	FIntPoint CellOf(const FVector2D& Position) const;

	float CellSize;
	TMap<FIntPoint, TArray<FEntry>> Cells;
};
