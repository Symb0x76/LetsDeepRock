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

	const int32 NewWeaponIndex = EquipmentState.WeaponInventory.IndexOfByKey(InWeaponData);
	if (NewWeaponIndex == INDEX_NONE)
	{
		return;
	}
	EquipmentState.CurrentWeaponIndex = NewWeaponIndex;

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
	return EquipmentState.WeaponInventory.IsValidIndex(EquipmentState.CurrentWeaponIndex) ?
		EquipmentState.WeaponInventory[EquipmentState.CurrentWeaponIndex] : nullptr;
}

void UWeaponEquipmentComponent::OnRep_EquipmentState()
{
	ensureMsgf(
		EquipmentState.CurrentWeaponIndex == INDEX_NONE ||
		EquipmentState.WeaponInventory.IsValidIndex(EquipmentState.CurrentWeaponIndex),
		TEXT("Replicated weapon index is outside the replicated inventory."));
	NotifyEquipmentChanged();
}

void UWeaponEquipmentComponent::NotifyEquipmentChanged()
{
	OnEquipmentChanged.Broadcast(EquipmentState.CurrentWeaponIndex);
}

void UWeaponEquipmentComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UWeaponEquipmentComponent, EquipmentState);
}

void UWeaponEquipmentComponent::ServerSwitchWeapon_Implementation(const int32 NewIndex)
{
	SwitchWeaponAuthority(NewIndex);
}

void UWeaponEquipmentComponent::SwitchWeaponAuthority(const int32 NewIndex)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() ||
		!EquipmentState.WeaponInventory.IsValidIndex(NewIndex) ||
		NewIndex == EquipmentState.CurrentWeaponIndex)
	{
		return;
	}

	EquipWeapon(EquipmentState.WeaponInventory[NewIndex]);
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

// ---- 多武器库存与切换实现 ----

void UWeaponEquipmentComponent::SwitchWeapon(int32 NewIndex)
{
	if (!EquipmentState.WeaponInventory.IsValidIndex(NewIndex) ||
		NewIndex == EquipmentState.CurrentWeaponIndex)
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
	if (EquipmentState.WeaponInventory.Num() <= 1)
	{
		return;
	}

	const int32 NewIndex = (EquipmentState.CurrentWeaponIndex + 1) % EquipmentState.WeaponInventory.Num();
	SwitchWeapon(NewIndex);
}

void UWeaponEquipmentComponent::SwitchToPrevious()
{
	if (EquipmentState.WeaponInventory.Num() <= 1)
	{
		return;
	}

	const int32 NewIndex = (EquipmentState.CurrentWeaponIndex - 1 + EquipmentState.WeaponInventory.Num()) % EquipmentState.WeaponInventory.Num();
	SwitchWeapon(NewIndex);
}

void UWeaponEquipmentComponent::AddWeaponToInventory(UWeaponDataAsset* WeaponData)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !WeaponData)
	{
		return;
	}

	// 不重复添加
	if (EquipmentState.WeaponInventory.Contains(WeaponData))
	{
		return;
	}

	EquipmentState.WeaponInventory.Add(WeaponData);

	// 如果是第一把武器，自动装备
	if (EquipmentState.CurrentWeaponIndex == INDEX_NONE)
	{
		SwitchWeapon(0);
	}
	else
	{
		GetOwner()->ForceNetUpdate();
		NotifyEquipmentChanged();
	}
}

void UWeaponEquipmentComponent::RemoveWeaponFromInventory(UWeaponDataAsset* WeaponData)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() || !WeaponData)
	{
		return;
	}

	const int32 RemoveIndex = EquipmentState.WeaponInventory.IndexOfByKey(WeaponData);
	if (RemoveIndex == INDEX_NONE)
	{
		return;
	}

	// 如果移除的是当前装备的武器
	if (RemoveIndex == EquipmentState.CurrentWeaponIndex)
	{
		UnequipWeapon();
		EquipmentState.WeaponInventory.RemoveAt(RemoveIndex);

		// 库存还有武器则切到第 0 个，否则没有武器
		if (EquipmentState.WeaponInventory.Num() > 0)
		{
			SwitchWeapon(0);
		}
		else
		{
			EquipmentState.CurrentWeaponIndex = INDEX_NONE;
			GetOwner()->ForceNetUpdate();
			NotifyEquipmentChanged();
		}
	}
	else
	{
		// 移除的不是当前装备的武器，只从数组移除
		EquipmentState.WeaponInventory.RemoveAt(RemoveIndex);

		// 如果移除的武器索引在当前武器之前，需要前移索引
		if (RemoveIndex < EquipmentState.CurrentWeaponIndex)
		{
			--EquipmentState.CurrentWeaponIndex;
		}

		GetOwner()->ForceNetUpdate();
		NotifyEquipmentChanged();
	}
}
