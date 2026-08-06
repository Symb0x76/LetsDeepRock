// Copyright Epic Games, Inc. All Rights Reserved.


#include "NPCBase.h"
#include "AbilitySystemComponent.h"
#include "Abilities/GameplayAbility.h"
#include "Attributes/WeaponHealthSet.h"
#include "Attributes/WeaponResistanceSet.h"
#include "Attributes/WeaponMovementSet.h"
#include "Attributes/StatusEffectComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameplayTagContainer.h"
#include "TimerManager.h"
#include "WeaponGameplayTags.h"
#include "WeaponGameState.h"
#include "AIController.h"
#include "BrainComponent.h"

ANPCBase::ANPCBase()
{
	AbilitySystemComponent = CreateDefaultSubobject<UAbilitySystemComponent>(TEXT("AbilitySystemComponent"));
	AbilitySystemComponent->SetIsReplicated(true);
	AbilitySystemComponent->SetReplicationMode(EGameplayEffectReplicationMode::Minimal);

	HealthSet = CreateDefaultSubobject<UWeaponHealthSet>(TEXT("HealthSet"));
	ResistanceSet = CreateDefaultSubobject<UWeaponResistanceSet>(TEXT("ResistanceSet"));
	MovementSet = CreateDefaultSubobject<UWeaponMovementSet>(TEXT("MovementSet"));
	StatusEffectComponent = CreateDefaultSubobject<UStatusEffectComponent>(TEXT("StatusEffectComponent"));
}

void ANPCBase::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	if (StatusEffectComponent)
	{
		StatusEffectComponent->ClearAllEffects();
	}
	Super::EndPlay(EndPlayReason);
}

UAbilitySystemComponent* ANPCBase::GetAbilitySystemComponent() const
{
	return AbilitySystemComponent;
}

void ANPCBase::BeginPlay()
{
	Super::BeginPlay();

	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->InitAbilityActorInfo(this, this);
	}

	if (HasAuthority() && HealthSet)
	{
		HealthSet->InitMaxHealth(DefaultHealth);
		HealthSet->InitHealth(DefaultHealth);
		HealthSet->OnHealthDepleted.AddUObject(this, &ANPCBase::HandleHealthDepleted);
	}

	if (HasAuthority() && AbilitySystemComponent)
	{
		for (const TSubclassOf<UGameplayAbility>& Ability : DefaultAbilities)
		{
			if (Ability)
			{
				AbilitySystemComponent->GiveAbility(FGameplayAbilitySpec(Ability, 1, INDEX_NONE, this));
			}
		}
	}

	SetGameplayRecoveryBlocked(AWeaponGameState::IsGameplayRecoveryBlockedFor(this));
}

void ANPCBase::SetGameplayRecoveryBlocked(const bool bBlocked)
{
	if (!HasAuthority() || bGameplayRecoveryBlocked == bBlocked)
	{
		return;
	}
	bGameplayRecoveryBlocked = bBlocked;
	if (StatusEffectComponent)
	{
		StatusEffectComponent->SetGameplayRecoveryBlocked(bBlocked);
	}

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

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		if (bBlocked)
		{
			SavedRecoveryMovementMode = static_cast<uint8>(Movement->MovementMode);
			SavedRecoveryCustomMovementMode = Movement->CustomMovementMode;
			bSavedMovementModeForRecovery = true;
			Movement->DisableMovement();
			Movement->StopMovementImmediately();
		}
		else if (bSavedMovementModeForRecovery &&
			(!AbilitySystemComponent || !AbilitySystemComponent->HasMatchingGameplayTag(TAG_State_Dead)))
		{
			Movement->SetMovementMode(
				static_cast<EMovementMode>(SavedRecoveryMovementMode),
				SavedRecoveryCustomMovementMode);
		}
	}
	if (!bBlocked)
	{
		bSavedMovementModeForRecovery = false;
	}
}

void ANPCBase::HandleHealthDepleted(AActor* OwningActor)
{
	if (StatusEffectComponent)
	{
		StatusEffectComponent->ClearAllEffects();
	}
	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->AddLooseGameplayTag(TAG_State_Dead);
	}

	if (AAIController* AIC = Cast<AAIController>(GetController()))
	{
		if (UBrainComponent* Brain = AIC->GetBrainComponent())
		{
			Brain->StopLogic(TEXT("Dead"));
		}
	}

	GetCharacterMovement()->DisableMovement();
	GetCharacterMovement()->StopMovementImmediately();

	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	SetActorHiddenInGame(true);

	FTimerHandle DestroyTimerHandle;
	TWeakObjectPtr<ANPCBase> WeakThis(this);
	GetWorldTimerManager().SetTimer(
		DestroyTimerHandle,
		[WeakThis]()
		{
			if (WeakThis.IsValid())
			{
				WeakThis->Destroy();
			}
		},
		DeathLingerTime,
		false
	);
}
