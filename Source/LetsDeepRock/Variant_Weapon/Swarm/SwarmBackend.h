// Swarm 池化虫群：后端接口（Pooled / Mass 两种实现共用）

#pragma once

#include "CoreMinimal.h"
#include "Swarm/SwarmTypes.h"

class UWorld;

/** 虫群后端种类，由 Swarm.Backend 选择 */
enum class ESwarmBackendKind : uint8
{
	Pooled = 0,
	Mass = 1,
};

/** 虫群后端接口：仅服务端持有；房间归属由波次导演负责，快照经 ForEachAlive/SpawnAt 完成 */
class ISwarmBackend
{
public:
	virtual ~ISwarmBackend() = default;

	virtual ESwarmBackendKind GetKind() const = 0;
	virtual void Initialize(UWorld& World) = 0;
	virtual void Deinitialize() = 0;

	/** 返回实际生成数(池满时少于 Count) */
	virtual int32 SpawnAt(FName ArchetypeRow, const FVector& Location, int32 Count) = 0;

	/** 返回命中数 */
	virtual int32 ApplyAreaDamage(const FVector& Center, float Radius, const FSwarmDamageEvent& Event) = 0;

	/** 命中线段上第一只虫;返回 0 或 1 */
	virtual int32 ApplyLineHit(const FVector& Start, const FVector& End, const FSwarmDamageEvent& Event) = 0;

	virtual int32 CountAlive() const = 0;
	virtual void ForEachAlive(TFunctionRef<void(const FSwarmBugView&)> Fn) const = 0;
	virtual void DespawnAll() = 0;
};
