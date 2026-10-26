// Swarm 池化虫群：Pooled 后端实现

#include "Swarm/PooledSwarmBackend.h"

#include "Engine/World.h"
#include "Swarm/SwarmPoolActor.h"
#include "Swarm/SwarmSubsystem.h"

void FPooledSwarmBackend::Initialize(UWorld& World)
{
	FActorSpawnParameters Params;
	Params.SpawnCollisionHandlingOverride = ESpawnActorCollisionHandlingMethod::AlwaysSpawn;
	Params.ObjectFlags |= RF_Transient;

	ASwarmPoolActor* Pool = World.SpawnActor<ASwarmPoolActor>(ASwarmPoolActor::StaticClass(), FTransform::Identity, Params);
	if (!Pool)
	{
		UE_LOG(LogTemp, Error, TEXT("[Swarm] Failed to spawn pool actor"));
		return;
	}

	Pool->InitPool(USwarmSubsystem::GetConfiguredPoolSize());
	PoolActor = Pool;
}

void FPooledSwarmBackend::Deinitialize()
{
	ASwarmPoolActor* Pool = PoolActor.Get();
	PoolActor.Reset();
	if (!Pool)
	{
		return;
	}
	const UWorld* World = Pool->GetWorld();
	if (World && !World->bIsTearingDown)
	{
		Pool->Destroy();
	}
}

int32 FPooledSwarmBackend::SpawnAt(const FName ArchetypeRow, const FVector& Location, const int32 Count)
{
	ASwarmPoolActor* Pool = PoolActor.Get();
	return Pool ? Pool->SpawnBugs(ArchetypeRow, Location, Count) : 0;
}

int32 FPooledSwarmBackend::ApplyAreaDamage(const FVector& Center, const float Radius, const FSwarmDamageEvent& Event)
{
	ASwarmPoolActor* Pool = PoolActor.Get();
	return Pool ? Pool->DamageInRadius(Center, Radius, Event) : 0;
}

int32 FPooledSwarmBackend::ApplyLineHit(const FVector& Start, const FVector& End, const FSwarmDamageEvent& Event)
{
	ASwarmPoolActor* Pool = PoolActor.Get();
	return Pool ? Pool->DamageFirstOnLine(Start, End, Event) : 0;
}

int32 FPooledSwarmBackend::CountAlive() const
{
	const ASwarmPoolActor* Pool = PoolActor.Get();
	return Pool ? Pool->NumAlive() : 0;
}

int32 FPooledSwarmBackend::GetCapacity() const
{
	const ASwarmPoolActor* Pool = PoolActor.Get();
	return Pool ? Pool->GetCapacity() : 0;
}

void FPooledSwarmBackend::ForEachAlive(TFunctionRef<void(const FSwarmBugView&)> Fn) const
{
	if (const ASwarmPoolActor* Pool = PoolActor.Get())
	{
		Pool->ForEachAliveView(Fn);
	}
}

void FPooledSwarmBackend::DespawnAll()
{
	if (ASwarmPoolActor* Pool = PoolActor.Get())
	{
		Pool->DespawnAll();
	}
}
