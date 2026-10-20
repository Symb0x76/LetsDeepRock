// Copyright Epic Games, Inc. All Rights Reserved.


#include "Drone.h"
#include "Abilities/WeaponGameplayCues.h"
#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "Attributes/WeaponHealthSet.h"
#include "Data/DataTableRows.h"
#include "GameDataSubsystem.h"
#include "WeaponGameplayTags.h"
#include "WeaponGameState.h"
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
	if (AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}
	OrbitTarget = Target;
}

void ADrone::SetGameplayRecoveryBlocked(const bool bBlocked)
{
	if (!HasAuthority() || bGameplayRecoveryBlocked == bBlocked)
	{
		return;
	}
	bGameplayRecoveryBlocked = bBlocked;

	const FGameplayTagContainer WeaponAbilityTags(TAG_Ability_Weapon);
	if (AbilitySystemComponent)
	{
		if (bBlocked)
		{
			AbilitySystemComponent->CancelAbilities(&WeaponAbilityTags);
			AbilitySystemComponent->BlockAbilitiesWithTags(WeaponAbilityTags);
		}
		else
		{
			AbilitySystemComponent->UnBlockAbilitiesWithTags(WeaponAbilityTags);
		}
	}

	if (bBlocked)
	{
		bTickWasEnabledBeforeRecovery = IsActorTickEnabled();
		SetActorTickEnabled(false);
		GetWorldTimerManager().PauseTimer(LifetimeTimerHandle);
	}
	else
	{
		SetActorTickEnabled(bTickWasEnabledBeforeRecovery);
		GetWorldTimerManager().UnPauseTimer(LifetimeTimerHandle);
		bTickWasEnabledBeforeRecovery = false;
	}
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

	SetGameplayRecoveryBlocked(AWeaponGameState::IsGameplayRecoveryBlockedFor(this));
}

void ADrone::InitializeFromConfig(const FDroneConfigRow& Config)
{
	RemainingLifetime = Config.Duration;

	if (Config.Duration > 0.0f)
	{
		TWeakObjectPtr<ADrone> WeakThis(this);
		GetWorldTimerManager().SetTimer(
			LifetimeTimerHandle,
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
	if (AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}

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

void ADrone::HandleGameplayCue(AActor* Self, FGameplayTag GameplayCueTag, EGameplayCueEvent::Type EventType, const FGameplayCueParameters& Parameters)
{
	WeaponGameplayCues::HandleCue(Self, GameplayCueTag, EventType, Parameters);
}
