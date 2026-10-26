// Swarm 池化虫群：Pooled 后端（持有 ASwarmPoolActor 并转发接口调用）

#pragma once

#include "CoreMinimal.h"
#include "Swarm/SwarmBackend.h"
#include "UObject/WeakObjectPtrTemplates.h"

class ASwarmPoolActor;

/** Pooled 后端：Initialize 时在服务器生成池 Actor，Deinitialize 时销毁 */
class FPooledSwarmBackend final : public ISwarmBackend
{
public:
	virtual ESwarmBackendKind GetKind() const override { return ESwarmBackendKind::Pooled; }
	virtual void Initialize(UWorld& World) override;
	virtual void Deinitialize() override;

	virtual int32 SpawnAt(FName ArchetypeRow, const FVector& Location, int32 Count) override;
	virtual int32 ApplyAreaDamage(const FVector& Center, float Radius, const FSwarmDamageEvent& Event) override;
	virtual int32 ApplyLineHit(const FVector& Start, const FVector& End, const FSwarmDamageEvent& Event) override;
	virtual int32 CountAlive() const override;
	virtual int32 GetCapacity() const override;
	virtual void ForEachAlive(TFunctionRef<void(const FSwarmBugView&)> Fn) const override;
	virtual void DespawnAll() override;

private:
	TWeakObjectPtr<ASwarmPoolActor> PoolActor;
};
