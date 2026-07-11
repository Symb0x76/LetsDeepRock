// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponMovementSet.h"
#include "Net/UnrealNetwork.h"

UWeaponMovementSet::UWeaponMovementSet()
{
	MoveSpeed = 1.0f;
	SprintSpeed = 1.0f;
	DodgeDistance = 600.0f;
}

void UWeaponMovementSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	if (Attribute == GetMoveSpeedAttribute())
	{
		NewValue = FMath::Max(NewValue, 0.0f);
	}
	else if (Attribute == GetSprintSpeedAttribute())
	{
		NewValue = FMath::Max(NewValue, 0.0f);
	}
	else if (Attribute == GetDodgeDistanceAttribute())
	{
		NewValue = FMath::Max(NewValue, 0.0f);
	}
}

void UWeaponMovementSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponMovementSet, MoveSpeed, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponMovementSet, SprintSpeed, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponMovementSet, DodgeDistance, COND_None, REPNOTIFY_Always);
}

void UWeaponMovementSet::OnRep_MoveSpeed(const FGameplayAttributeData& OldMoveSpeed)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponMovementSet, MoveSpeed, OldMoveSpeed);
}

void UWeaponMovementSet::OnRep_SprintSpeed(const FGameplayAttributeData& OldSprintSpeed)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponMovementSet, SprintSpeed, OldSprintSpeed);
}

void UWeaponMovementSet::OnRep_DodgeDistance(const FGameplayAttributeData& OldDodgeDistance)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponMovementSet, DodgeDistance, OldDodgeDistance);
}
