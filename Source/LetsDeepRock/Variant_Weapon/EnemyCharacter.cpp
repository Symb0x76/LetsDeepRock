// Copyright Epic Games, Inc. All Rights Reserved.


#include "EnemyCharacter.h"
#include "AbilitySystemComponent.h"
#include "Attributes/AttributeInitHelper.h"
#include "Attributes/WeaponHealthSet.h"
#include "GameDataSubsystem.h"
#include "AI/EnemyAIController.h"
#include "WeaponGameplayTags.h"

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
					FEnemyConfigRow AttributeConfig = *Config;
					if (ActorHasTag(TEXT("WeaponDamageTestTarget")))
					{
						// 仅覆盖属性初始化副本的最大生命值，保留护甲、抗性等原配置。
						AttributeConfig.MaxHealth = 999999.0f;
					}

					FAttributeInitHelper::InitializeNPCAttributes(AbilitySystemComponent, AttributeConfig);
					DeathLingerTime = Config->DeathLingerTime;

					if (ActorHasTag(TEXT("WeaponDamageTestTarget")))
					{
						UE_LOG(LogTemp, Warning, TEXT("[ArmorDiag] Init Actor=%s Row=%s MaxHealth=%.3f Health=%.3f Armor=%.3f ArmorTypeTag=%s Unarmored=%s Light=%s Heavy=%s"),
							*GetNameSafe(this),
							*EnemyConfigRowName.ToString(),
							HealthSet->GetMaxHealth(),
							HealthSet->GetHealth(),
							HealthSet->GetArmor(),
							*AttributeConfig.ArmorTypeTag.ToString(),
							AbilitySystemComponent->HasMatchingGameplayTag(TAG_ArmorType_Unarmored) ? TEXT("true") : TEXT("false"),
							AbilitySystemComponent->HasMatchingGameplayTag(TAG_ArmorType_Light) ? TEXT("true") : TEXT("false"),
							AbilitySystemComponent->HasMatchingGameplayTag(TAG_ArmorType_Heavy) ? TEXT("true") : TEXT("false"));
					}
				}
			}
		}
	}
}
