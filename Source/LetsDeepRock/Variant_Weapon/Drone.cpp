// Copyright Epic Games, Inc. All Rights Reserved.


#include "Drone.h"
#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "Attributes/WeaponHealthSet.h"
#include "Data/DataTableRows.h"
#include "GameDataSubsystem.h"
#include "WeaponGameplayTags.h"
#include "TimerManager.h"
#include "Engine/World.h"

ADrone::ADrone()
{
	PrimaryActorTick.bCanEverTick = true;

	AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Minimal);

	HealthSet = CreateDefaultSubobject<UWeaponHealthSet>(TEXT("HealthSet"));
}

UAbilitySystemComponent* ADrone::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

void ADrone::SetOrbitTarget(AActor* Target)
{
	OrbitTarget = Target;
}

void ADrone::BeginPlay()
{
	Super::BeginPlay();

	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->InitAbilityActorInfo(this, this);
	}

	if (HasAuthority())
	{
		if (HealthSet)
		{
			HealthSet->OnHealthDepleted.AddUObject(this, &ADrone::HandleHealthDepleted);
		}

		if (DroneConfigRowName != NAME_None)
		{
			if (UGameInstance* GI = GetGameInstance())
			{
				if (UGameDataSubsystem* DataSys = GI->GetSubsystem<UGameDataSubsystem>())
				{
					if (const FDroneConfigRow* Config = DataSys->GetDroneConfig(DroneConfigRowName))
					{
						InitializeFromConfig(*Config);
					}
				}
			}
		}

		if (AbilitySystemComponent)
		{
			for (const TSubclassOf<UGameplayAbility>& Ability : DefaultAbilities)
			{
				if (Ability)
				{
					AbilitySystemComponent->GiveAbility(FGameplayAbilitySpec(Ability, 1, INDEX_NONE, this));
				}
			}
		}
	}
}

void ADrone::InitializeFromConfig(const FDroneConfigRow& Config)
{
	RemainingLifetime = Config.Duration;

	if (Config.Duration > 0.0f)
	{
		TWeakObjectPtr<ADrone> WeakThis(this);
		FTimerHandle LifetimeHandle;
		GetWorldTimerManager().SetTimer(
			LifetimeHandle,
			[WeakThis]()
			{
				if (WeakThis.IsValid())
				{
					WeakThis->HandleLifetimeExpired();
				}
			},
			Config.Duration,
			false
		);
	}

	if (HealthSet)
	{
		HealthSet->InitMaxHealth(1.0f);
		HealthSet->InitHealth(1.0f);
	}
}

void ADrone::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	if (OrbitTarget.Get())
	{
		// TODO: 从 DT_DroneConfig 读取 OrbitRadius / OrbitSpeed / HoverHeight，
		//       实现完整轨道运动逻辑
		const FVector TargetLocation = OrbitTarget->GetActorLocation();
		SetActorLocation(TargetLocation);
	}
}

void ADrone::HandleHealthDepleted(AActor* OwningActor)
{
	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->AddLooseGameplayTag(TAG_State_Dead);
	}

	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);

	Destroy();
}

void ADrone::HandleLifetimeExpired()
{
	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->AddLooseGameplayTag(TAG_State_Dead);
	}

	SetActorHiddenInGame(true);
	SetActorEnableCollision(false);

	Destroy();
}
