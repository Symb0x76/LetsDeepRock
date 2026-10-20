// Copyright Epic Games, Inc. All Rights Reserved.

#include "StatusEffectComponent.h"

#include "AbilitySystemBlueprintLibrary.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "Data/GameDataSubsystem.h"
#include "Engine/GameInstance.h"
#include "GameplayEffect.h"
#include "GameplayEffectTypes.h"
#include "StatusEffectGameplayEffect.h"
#include "TimerManager.h"
#include "WeaponGameplayTags.h"

UStatusEffectComponent::UStatusEffectComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
}

UAbilitySystemComponent* UStatusEffectComponent::GetASC() const
{
	const IAbilitySystemInterface* ASCInterface = Cast<IAbilitySystemInterface>(GetOwner());
	return ASCInterface ? ASCInterface->GetAbilitySystemComponent() : nullptr;
}

bool UStatusEffectComponent::TryApply(const FName StatusEffectId, TSubclassOf<UGameplayEffect> DamageEffectClass)
{
	AActor* Owner = GetOwner();
	UAbilitySystemComponent* ASC = GetASC();
	if (!Owner || !Owner->HasAuthority() || !ASC || bGameplayRecoveryBlocked || StatusEffectId.IsNone() || !DamageEffectClass)
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

	FAppliedStatusEffect& Effect = ActiveEffects.FindOrAdd(StatusEffectId);
	Effect.Config = *Config;
	Effect.DamageEffectClass = DamageEffectClass;
	++Effect.PendingHits;
	if (Effect.PendingHits < FMath::Max(1, Config->HitsPerStack))
	{
		return true;
	}
	Effect.PendingHits = 0;

	// 叠层/时长/衰减全部交给 GAS：重复应用同一行缓存实例即自动叠层并刷新时长；
	// 到期走 RemoveSingleStackAndRefreshDuration（掉一层），掉光自动移除。
	if (UStatusEffectGameplayEffect* Def = UStatusEffectGameplayEffect::GetOrCreate(StatusEffectId, *Config))
	{
		FGameplayEffectSpec Spec(Def, ASC->MakeEffectContext(), 1.0f);
		// Duration=0 的旧行为是"立即开始 2 秒/层衰减"，以 2 秒时长近似。
		const float Duration = Config->Duration > 0.0f ? Config->Duration : 2.0f;
		Spec.SetDuration(Duration, true);
		Effect.Handle = ASC->ApplyGameplayEffectSpecToSelf(Spec);
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
	UAbilitySystemComponent* ASC = GetASC();
	if (!ASC)
	{
		return 1.0f;
	}

	float Bonus = 0.0f;
	for (const TPair<FName, FAppliedStatusEffect>& Pair : ActiveEffects)
	{
		if (!IsEffectType(Pair.Key, Pair.Value.Config, TEXT("Vulnerable")))
		{
			continue;
		}
		int32 Stacks = 0;
		if (const FActiveGameplayEffect* Active =
			ASC->GetActiveGameplayEffects().GetActiveGameplayEffect(Pair.Value.Handle))
		{
			Stacks = Active->Spec.GetStackCount();
		}
		if (Stacks > 0)
		{
			Bonus += Pair.Value.Config.Magnitude * Stacks;
		}
	}
	return 1.0f + Bonus;
}

void UStatusEffectComponent::TickDamage(const FName StatusEffectId)
{
	FAppliedStatusEffect* Effect = ActiveEffects.Find(StatusEffectId);
	UAbilitySystemComponent* ASC = GetASC();

	// GE 已自然到期掉光/被移除：惰性清理本地条目。
	if (!Effect || !ASC || !Effect->Handle.IsValid() ||
		!ASC->GetActiveGameplayEffects().GetActiveGameplayEffect(Effect->Handle))
	{
		RemoveEffect(StatusEffectId);
		return;
	}

	const FActiveGameplayEffect* Active = ASC->GetActiveGameplayEffects().GetActiveGameplayEffect(Effect->Handle);
	const int32 Stacks = Active ? Active->Spec.GetStackCount() : 0;
	if (Stacks <= 0 || !Effect->DamageEffectClass)
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
	const float Damage = Effect->Config.DamagePerTick * (bBleeding ? Stacks : 1);
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
	// 已知行为差异：GE 的时长/衰减在屏障期间继续走（不可暂停），
	// 旧实现连 DurationTimer 一起冻结。屏障窗口内伤害本身已被
	// DamageExecCalculation 的屏障早退挡住，此处只需停周期伤害发射器。
	for (TPair<FName, FAppliedStatusEffect>& Pair : ActiveEffects)
	{
		if (bBlocked)
		{
			GetWorld()->GetTimerManager().PauseTimer(Pair.Value.TickTimer);
		}
		else
		{
			GetWorld()->GetTimerManager().UnPauseTimer(Pair.Value.TickTimer);
		}
	}
}

void UStatusEffectComponent::CaptureSnapshotState(TArray<FStatusEffectSnapshotEntry>& OutEntries) const
{
	OutEntries.Reset();
	const UAbilitySystemComponent* ASC = GetASC();
	const UWorld* World = GetWorld();
	if (!ASC || !World)
	{
		return;
	}
	for (const TPair<FName, FAppliedStatusEffect>& Pair : ActiveEffects)
	{
		const FActiveGameplayEffect* Active = Pair.Value.Handle.IsValid()
			? ASC->GetActiveGameplayEffects().GetActiveGameplayEffect(Pair.Value.Handle)
			: nullptr;
		// 只有 PendingHits、尚未成层的条目没有 GE;快照只保留真正生效的状态。
		if (!Active || Active->Spec.GetStackCount() <= 0 || !Pair.Value.DamageEffectClass)
		{
			continue;
		}
		FStatusEffectSnapshotEntry& Entry = OutEntries.AddDefaulted_GetRef();
		Entry.StatusEffectId = Pair.Key;
		Entry.StackCount = Active->Spec.GetStackCount();
		Entry.RemainingDuration = FMath::Max(0.0f, Active->GetTimeRemaining(World->GetTimeSeconds()));
		Entry.PendingHits = Pair.Value.PendingHits;
		Entry.DamageEffectClass = Pair.Value.DamageEffectClass;
	}
}

bool UStatusEffectComponent::RestoreSnapshotEntry(const FStatusEffectSnapshotEntry& Entry)
{
	AActor* Owner = GetOwner();
	UAbilitySystemComponent* ASC = GetASC();
	UWorld* World = GetWorld();
	if (!Owner || !Owner->HasAuthority() || !ASC || !World || Entry.StatusEffectId.IsNone() ||
		!Entry.DamageEffectClass || Entry.StackCount <= 0 || Entry.RemainingDuration <= 0.0f)
	{
		return false;
	}

	const UGameInstance* GameInstance = World->GetGameInstance();
	const UGameDataSubsystem* DataSubsystem = GameInstance ? GameInstance->GetSubsystem<UGameDataSubsystem>() : nullptr;
	const FStatusEffectConfigRow* Config = DataSubsystem ? DataSubsystem->GetStatusEffectConfig(Entry.StatusEffectId) : nullptr;
	UStatusEffectGameplayEffect* Def = Config ? UStatusEffectGameplayEffect::GetOrCreate(Entry.StatusEffectId, *Config) : nullptr;
	if (!Def)
	{
		return false;
	}

	RemoveEffect(Entry.StatusEffectId);
	FAppliedStatusEffect& Effect = ActiveEffects.Add(Entry.StatusEffectId);
	Effect.Config = *Config;
	Effect.DamageEffectClass = Entry.DamageEffectClass;
	Effect.PendingHits = Entry.PendingHits;

	FGameplayEffectSpec Spec(Def, ASC->MakeEffectContext(), 1.0f);
	Spec.SetDuration(Entry.RemainingDuration, true);
	Spec.SetStackCount(FMath::Clamp(Entry.StackCount, 1, FMath::Max(Config->MaxStacks, 1)));
	Effect.Handle = ASC->ApplyGameplayEffectSpecToSelf(Spec);
	if (!Effect.Handle.IsValid())
	{
		ActiveEffects.Remove(Entry.StatusEffectId);
		return false;
	}

	if (Config->DamagePerTick > 0.0f && Config->TickInterval > 0.0f)
	{
		World->GetTimerManager().SetTimer(
			Effect.TickTimer,
			FTimerDelegate::CreateUObject(this, &UStatusEffectComponent::TickDamage, Entry.StatusEffectId),
			Config->TickInterval,
			true);
		if (bGameplayRecoveryBlocked)
		{
			World->GetTimerManager().PauseTimer(Effect.TickTimer);
		}
	}
	return true;
}

void UStatusEffectComponent::RemoveEffect(const FName StatusEffectId)
{
	if (FAppliedStatusEffect* Effect = ActiveEffects.Find(StatusEffectId))
	{
		GetWorld()->GetTimerManager().ClearTimer(Effect->TickTimer);
		if (UAbilitySystemComponent* ASC = GetASC(); ASC && Effect->Handle.IsValid())
		{
			ASC->RemoveActiveGameplayEffect(Effect->Handle);
		}
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
