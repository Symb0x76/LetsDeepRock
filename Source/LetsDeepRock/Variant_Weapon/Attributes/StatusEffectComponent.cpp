// Copyright Epic Games, Inc. All Rights Reserved.

#include "StatusEffectComponent.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "Data/GameDataSubsystem.h"
#include "Engine/GameInstance.h"
#include "GameplayEffect.h"
#include "GameplayEffectTypes.h"
#include "TimerManager.h"
#include "WeaponGameplayTags.h"

UStatusEffectComponent::UStatusEffectComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

bool UStatusEffectComponent::TryApply(const FName StatusEffectId, TSubclassOf<UGameplayEffect> DamageEffectClass)
{
	AActor* Owner = GetOwner();
	if (!Owner || !Owner->HasAuthority() || bGameplayRecoveryBlocked || StatusEffectId.IsNone() || !DamageEffectClass)
	{
		return false;
	}

	const UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	const UGameDataSubsystem* DataSubsystem = GameInstance ? GameInstance->GetSubsystem<UGameDataSubsystem>() : nullptr;
	const FStatusEffectConfigRow* Config = DataSubsystem ? DataSubsystem->GetStatusEffectConfig(StatusEffectId) : nullptr;
	if (!Config || FMath::FRand() >= FMath::Clamp(Config->ApplyChance, 0.0f, 1.0f))
	{
		return false;
	}

	FActiveStatusEffect& Effect = ActiveEffects.FindOrAdd(StatusEffectId);
	Effect.Config = *Config;
	Effect.DamageEffectClass = DamageEffectClass;
	++Effect.PendingHits;
	if (Effect.PendingHits < FMath::Max(1, Config->HitsPerStack))
	{
		return true;
	}

	Effect.PendingHits = 0;
	Effect.Stacks = FMath::Min(Effect.Stacks + 1, FMath::Max(1, Config->MaxStacks));
	GetWorld()->GetTimerManager().ClearTimer(Effect.DecayTimer);
	GetWorld()->GetTimerManager().ClearTimer(Effect.DurationTimer);
	if (Config->Duration > 0.0f)
	{
		GetWorld()->GetTimerManager().SetTimer(
			Effect.DurationTimer,
			FTimerDelegate::CreateUObject(this, &UStatusEffectComponent::BeginDecay, StatusEffectId),
			Config->Duration,
			false);
	}
	else
	{
		BeginDecay(StatusEffectId);
	}

	if (Config->DamagePerTick > 0.0f && Config->TickInterval > 0.0f &&
		!GetWorld()->GetTimerManager().IsTimerActive(Effect.TickTimer))
	{
		GetWorld()->GetTimerManager().SetTimer(
			Effect.TickTimer,
			FTimerDelegate::CreateUObject(this, &UStatusEffectComponent::TickDamage, StatusEffectId),
			Config->TickInterval,
			true);
	}
	return true;
}

float UStatusEffectComponent::GetVulnerableMultiplier() const
{
	float Bonus = 0.0f;
	for (const TPair<FName, FActiveStatusEffect>& Pair : ActiveEffects)
	{
		if (Pair.Value.Stacks > 0 && IsEffectType(Pair.Key, Pair.Value.Config, TEXT("Vulnerable")))
		{
			Bonus += Pair.Value.Config.Magnitude * Pair.Value.Stacks;
		}
	}
	return 1.0f + Bonus;
}

void UStatusEffectComponent::BeginDecay(const FName StatusEffectId)
{
	if (FActiveStatusEffect* Effect = ActiveEffects.Find(StatusEffectId))
	{
		GetWorld()->GetTimerManager().SetTimer(
			Effect->DecayTimer,
			FTimerDelegate::CreateUObject(this, &UStatusEffectComponent::DecayStack, StatusEffectId),
			2.0f,
			true);
	}
}

void UStatusEffectComponent::DecayStack(const FName StatusEffectId)
{
	FActiveStatusEffect* Effect = ActiveEffects.Find(StatusEffectId);
	if (Effect && --Effect->Stacks <= 0)
	{
		RemoveEffect(StatusEffectId);
	}
}

void UStatusEffectComponent::TickDamage(const FName StatusEffectId)
{
	FActiveStatusEffect* Effect = ActiveEffects.Find(StatusEffectId);
	IAbilitySystemInterface* ASCInterface = Cast<IAbilitySystemInterface>(GetOwner());
	UAbilitySystemComponent* ASC = ASCInterface ? ASCInterface->GetAbilitySystemComponent() : nullptr;
	if (!Effect || Effect->Stacks <= 0 || !ASC || !Effect->DamageEffectClass)
	{
		return;
	}

	const bool bBleeding = IsEffectType(StatusEffectId, Effect->Config, TEXT("Bleeding"));
	const bool bBurning = IsEffectType(StatusEffectId, Effect->Config, TEXT("Burning"));
	if (!bBleeding && !bBurning)
	{
		return;
	}

	FGameplayEffectContextHandle Context(new FGameplayEffectContext());
	FGameplayEffectSpecHandle Spec = ASC->MakeOutgoingSpec(Effect->DamageEffectClass, 1.0f, Context);
	if (!Spec.IsValid())
	{
		return;
	}
	const float Damage = Effect->Config.DamagePerTick * (bBleeding ? Effect->Stacks : 1);
	UAbilitySystemBlueprintLibrary::AssignTagSetByCallerMagnitude(Spec, TAG_Data_Damage, Damage);
	Spec.Data->AddDynamicAssetTag(bBleeding ? TAG_Damage_Status_Bleeding : TAG_Damage_Status_Burning);
	if (bBurning)
	{
		Spec.Data->AddDynamicAssetTag(TAG_Damage_Type_Fire);
	}
	ASC->ApplyGameplayEffectSpecToSelf(*Spec.Data.Get());
}

void UStatusEffectComponent::SetGameplayRecoveryBlocked(const bool bBlocked)
{
	if (bGameplayRecoveryBlocked == bBlocked)
	{
		return;
	}
	bGameplayRecoveryBlocked = bBlocked;
	for (TPair<FName, FActiveStatusEffect>& Pair : ActiveEffects)
	{
		if (bBlocked)
		{
			GetWorld()->GetTimerManager().PauseTimer(Pair.Value.DurationTimer);
			GetWorld()->GetTimerManager().PauseTimer(Pair.Value.DecayTimer);
			GetWorld()->GetTimerManager().PauseTimer(Pair.Value.TickTimer);
		}
		else
		{
			GetWorld()->GetTimerManager().UnPauseTimer(Pair.Value.DurationTimer);
			GetWorld()->GetTimerManager().UnPauseTimer(Pair.Value.DecayTimer);
			GetWorld()->GetTimerManager().UnPauseTimer(Pair.Value.TickTimer);
		}
	}
}

void UStatusEffectComponent::RemoveEffect(const FName StatusEffectId)
{
	if (FActiveStatusEffect* Effect = ActiveEffects.Find(StatusEffectId))
	{
		GetWorld()->GetTimerManager().ClearTimer(Effect->DurationTimer);
		GetWorld()->GetTimerManager().ClearTimer(Effect->DecayTimer);
		GetWorld()->GetTimerManager().ClearTimer(Effect->TickTimer);
	}
	ActiveEffects.Remove(StatusEffectId);
}

void UStatusEffectComponent::ClearAllEffects()
{
	TArray<FName> EffectIds;
	ActiveEffects.GetKeys(EffectIds);
	for (const FName EffectId : EffectIds)
	{
		RemoveEffect(EffectId);
	}
}

void UStatusEffectComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	ClearAllEffects();
	Super::EndPlay(EndPlayReason);
}

bool UStatusEffectComponent::IsEffectType(const FName StatusEffectId, const FStatusEffectConfigRow& Config, const TCHAR* TypeName)
{
	return StatusEffectId.ToString().Contains(TypeName, ESearchCase::IgnoreCase) ||
		Config.StatusTag.ToString().Contains(TypeName, ESearchCase::IgnoreCase);
}
