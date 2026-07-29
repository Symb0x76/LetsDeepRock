// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponEquipmentComponent.h"
#include "WeaponDataAsset.h"
#include "WeaponBase.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "Engine/World.h"
#include "GameFramework/Character.h"
#include "Net/UnrealNetwork.h"

UWeaponEquipmentComponent::UWeaponEquipmentComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UWeaponEquipmentComponent::BeginPlay()
{
	Super::BeginPlay();

	// 不在 BeginPlay 中自动装备：玩家 ASC 挂在 PlayerState 上，
	// 此时 PossessedBy 可能尚未执行、ASC 未初始化。
	// 改由 WeaponCharacter::PossessedBy() 完成 InitAbilitySystem() 后显式调用 EquipWeapon。
}

void UWeaponEquipmentComponent::EquipWeapon(UWeaponDataAsset* InWeaponData)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !InWeaponData)
	{
		return;
	}

	// 先卸载旧武器
	if (GetCurrentWeaponData())
	{
		UnequipWeapon();
	}

	const int32 NewWeaponIndex = DefaultWeapons.IndexOfByKey(InWeaponData);
	if (NewWeaponIndex == INDEX_NONE)
	{
		return;
	}
	CurrentWeaponIndex = NewWeaponIndex;

	UAbilitySystemComponent* ASC = GetOwnerASC();

	// 1. 授予射击 Ability
	if (ASC && InWeaponData->FireAbilityClass)
	{
		FGameplayAbilitySpec AbilitySpec(InWeaponData->FireAbilityClass, 1, INDEX_NONE, this);
		FireAbilityHandle = ASC->GiveAbility(AbilitySpec);
	}

	// 1b. 授予近战 Ability
	if (ASC && InWeaponData->MeleeAbilityClass)
	{
		FGameplayAbilitySpec MeleeSpec(InWeaponData->MeleeAbilityClass, 1, INDEX_NONE, this);
		MeleeAbilityHandle = ASC->GiveAbility(MeleeSpec);
	}

	// 2. Spawn 武器 Actor 并 Attach 到角色（可选：如果没有配武器网格则不 Spawn）
	// 俯视角本期可能没有角色骨骼/武器外观需求，WeaponActor 纯粹是可选的视觉表现
	if (!InWeaponData->WeaponMesh.IsNull())
	{
		if (UWorld* World = GetWorld())
		{
			FActorSpawnParameters SpawnParams;
			SpawnParams.Owner = GetOwner();

			CurrentWeaponActor = World->SpawnActor<AWeaponBase>(AWeaponBase::StaticClass(), FVector::ZeroVector, FRotator::ZeroRotator, SpawnParams);
			if (CurrentWeaponActor)
			{
				CurrentWeaponActor->InitFromDataAsset(InWeaponData);
				CurrentWeaponActor->AttachToActor(GetOwner(), FAttachmentTransformRules::SnapToTargetNotIncludingScale);
			}
		}
	}

	GetOwner()->ForceNetUpdate();
	NotifyEquipmentChanged();
}

UWeaponDataAsset* UWeaponEquipmentComponent::GetCurrentWeaponData() const
{
	return DefaultWeapons.IsValidIndex(CurrentWeaponIndex) ? DefaultWeapons[CurrentWeaponIndex] : nullptr;
}

void UWeaponEquipmentComponent::OnRep_CurrentWeaponIndex()
{
	ensureMsgf(
		CurrentWeaponIndex == INDEX_NONE || DefaultWeapons.IsValidIndex(CurrentWeaponIndex),
		TEXT("Replicated weapon index is outside DefaultWeapons."));
	NotifyEquipmentChanged();
}

void UWeaponEquipmentComponent::NotifyEquipmentChanged()
{
	OnEquipmentChanged.Broadcast(CurrentWeaponIndex);
}

void UWeaponEquipmentComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UWeaponEquipmentComponent, CurrentWeaponIndex);
}

void UWeaponEquipmentComponent::ServerSwitchWeapon_Implementation(const int32 NewIndex)
{
	SwitchWeaponAuthority(NewIndex);
}

void UWeaponEquipmentComponent::SwitchWeaponAuthority(const int32 NewIndex)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() ||
		!DefaultWeapons.IsValidIndex(NewIndex) ||
		NewIndex == CurrentWeaponIndex)
	{
		return;
	}

	EquipWeapon(DefaultWeapons[NewIndex]);
}

void UWeaponEquipmentComponent::UnequipWeapon()
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	UAbilitySystemComponent* ASC = GetOwnerASC();

	// 1. 移除射击 Ability
	if (FireAbilityHandle.IsValid())
	{
		if (ASC)
		{
			ASC->ClearAbility(FireAbilityHandle);
		}
		FireAbilityHandle = FGameplayAbilitySpecHandle();
	}

	// 2. 移除近战 Ability
	if (MeleeAbilityHandle.IsValid())
	{
		if (ASC)
		{
			ASC->ClearAbility(MeleeAbilityHandle);
		}
		MeleeAbilityHandle = FGameplayAbilitySpecHandle();
	}

	// 3. 销毁武器 Actor
	if (CurrentWeaponActor)
	{
		CurrentWeaponActor->Destroy();
		CurrentWeaponActor = nullptr;
	}
}

UAbilitySystemComponent* UWeaponEquipmentComponent::GetOwnerASC() const
{
	if (IAbilitySystemInterface* ASCInterface = Cast<IAbilitySystemInterface>(GetOwner()))
	{
		return ASCInterface->GetAbilitySystemComponent();
	}
	return nullptr;
}

// ---- 固定武器表与切换实现 ----

void UWeaponEquipmentComponent::SwitchWeapon(int32 NewIndex)
{
	if (!DefaultWeapons.IsValidIndex(NewIndex) || NewIndex == CurrentWeaponIndex)
	{
		return;
	}

	if (!GetOwner())
	{
		return;
	}

	if (GetOwner()->HasAuthority())
	{
		SwitchWeaponAuthority(NewIndex);
	}
	else
	{
		ServerSwitchWeapon(NewIndex);
	}
}

void UWeaponEquipmentComponent::SwitchToNext()
{
	if (DefaultWeapons.Num() <= 1)
	{
		return;
	}

	const int32 NewIndex = (CurrentWeaponIndex + 1) % DefaultWeapons.Num();
	SwitchWeapon(NewIndex);
}

void UWeaponEquipmentComponent::SwitchToPrevious()
{
	if (DefaultWeapons.Num() <= 1)
	{
		return;
	}

	const int32 NewIndex = (CurrentWeaponIndex - 1 + DefaultWeapons.Num()) % DefaultWeapons.Num();
	SwitchWeapon(NewIndex);
}

bool UWeaponEquipmentComponent::RestoreWeaponSelection(const int32 SavedWeaponIndex)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() ||
		(SavedWeaponIndex != INDEX_NONE && !DefaultWeapons.IsValidIndex(SavedWeaponIndex)))
	{
		return false;
	}

	TSet<UWeaponDataAsset*> UniqueWeapons;
	for (UWeaponDataAsset* WeaponData : DefaultWeapons)
	{
		if (!WeaponData || UniqueWeapons.Contains(WeaponData))
		{
			return false;
		}
		UniqueWeapons.Add(WeaponData);
	}

	UnequipWeapon();
	CurrentWeaponIndex = INDEX_NONE;

	if (SavedWeaponIndex != INDEX_NONE)
	{
		EquipWeapon(DefaultWeapons[SavedWeaponIndex]);
	}
	else
	{
		GetOwner()->ForceNetUpdate();
		NotifyEquipmentChanged();
	}

	return true;
}
