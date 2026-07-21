#include "WeaponSnapshotRegistry.h"

#include "Engine/World.h"

bool UWeaponSnapshotRegistry::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && World->GetNetMode() != NM_Client;
}

void UWeaponSnapshotRegistry::Deinitialize()
{
	Reset();
	Super::Deinitialize();
}

bool UWeaponSnapshotRegistry::RegisterActor(const FWeaponStableEntityId& Id, AActor* Actor)
{
	if (!GetWorld() || GetWorld()->GetNetMode() == NM_Client || !Id.IsValid() || !IsValid(Actor))
	{
		return false;
	}

	if (const TWeakObjectPtr<AActor>* Existing = ActorsById.Find(Id))
	{
		return Existing->Get() == Actor;
	}

	if (IdsByActor.Contains(Actor))
	{
		return false;
	}

	ActorsById.Add(Id, Actor);
	IdsByActor.Add(Actor, Id);
	return true;
}

void UWeaponSnapshotRegistry::UnregisterActor(AActor* Actor)
{
	if (!IsValid(Actor))
	{
		return;
	}

	if (const FWeaponStableEntityId* Id = IdsByActor.Find(Actor))
	{
		ActorsById.Remove(*Id);
	}
	IdsByActor.Remove(Actor);
}

void UWeaponSnapshotRegistry::UnregisterId(const FWeaponStableEntityId& Id)
{
	if (const TWeakObjectPtr<AActor>* Actor = ActorsById.Find(Id))
	{
		IdsByActor.Remove(*Actor);
	}
	ActorsById.Remove(Id);
}

AActor* UWeaponSnapshotRegistry::FindActor(const FWeaponStableEntityId& Id) const
{
	const TWeakObjectPtr<AActor>* Actor = ActorsById.Find(Id);
	return Actor ? Actor->Get() : nullptr;
}

bool UWeaponSnapshotRegistry::FindId(const AActor* Actor, FWeaponStableEntityId& OutId) const
{
	const FWeaponStableEntityId* Id = IdsByActor.Find(const_cast<AActor*>(Actor));
	if (!Id)
	{
		return false;
	}

	OutId = *Id;
	return true;
}

bool UWeaponSnapshotRegistry::Contains(const FWeaponStableEntityId& Id) const
{
	return FindActor(Id) != nullptr;
}

void UWeaponSnapshotRegistry::Reset()
{
	ActorsById.Reset();
	IdsByActor.Reset();
}
