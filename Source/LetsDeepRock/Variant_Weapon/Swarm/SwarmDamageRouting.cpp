#include "Swarm/SwarmDamageRouting.h"

#include "NPCBase.h"
#include "Swarm/SwarmDamageable.h"

bool SwarmIsDamageInstigatorAllowed(const AActor* Instigator)
{
	return !(Instigator && Instigator->IsA<ANPCBase>());
}

bool SwarmTryRouteDamage(AActor* Target, const FSwarmDamageEvent& Event)
{
	ISwarmDamageable* Damageable = Cast<ISwarmDamageable>(Target);
	if (!Damageable)
	{
		return false;
	}
	if (SwarmIsDamageInstigatorAllowed(Event.Instigator.Get()))
	{
		Damageable->ReceiveSwarmDamage(Event);
	}
	return true;
}
