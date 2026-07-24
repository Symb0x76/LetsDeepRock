// Copyright Epic Games, Inc. All Rights Reserved.


#include "EnemyCharacter.h"
#include "AbilitySystemComponent.h"
#include "Attributes/AttributeInitHelper.h"
#include "Attributes/WeaponHealthSet.h"
#include "GameDataSubsystem.h"
#include "AI/EnemyAIController.h"

AEnemyCharacter::AEnemyCharacter()
{
	AIControllerClass = AEnemyAIController::StaticClass();
	AutoPossessAI = EAutoPossessAI::PlacedInWorldOrSpawned;
}

void AEnemyCharacter::BeginPlay()
{
	Super::BeginPlay();

	if (HasAuthority() && HealthSet && EnemyConfigRowName != NAME_None)
	{
		if (UGameInstance* GI = GetGameInstance())
		{
			if (UGameDataSubsystem* DataSys = GI->GetSubsystem<UGameDataSubsystem>())
			{
				if (const FEnemyConfigRow* Config = DataSys->GetEnemyConfig(EnemyConfigRowName))
				{
					FAttributeInitHelper::InitializeNPCAttributes(AbilitySystemComponent, *Config);
					DeathLingerTime = Config->DeathLingerTime;
				}
			}
		}
	}
}
