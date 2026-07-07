// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponEquipmentComponent.h"
#include "WeaponDataAsset.h"
#include "WeaponBase.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "GameFramework/Character.h"

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
	if (!InWeaponData)
	{
		return;
	}

	// 先卸载旧武器
	if (CurrentWeaponData)
	{
		UnequipWeapon();
	}

	CurrentWeaponData = InWeaponData;
	CurrentWeaponIndex = WeaponInventory.IndexOfByKey(InWeaponData);

	// 仅服务端授予 Ability 和 Spawn 武器 Actor
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

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
		UWorld* World = GetWorld();
		if (World)
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
}

void UWeaponEquipmentComponent::EquipWeaponLocal(UWeaponDataAsset* InWeaponData)
{
	// 客户端仅设置数据引用，用于 Fire() 中获取 FireAbilityClass
	// 不 GiveAbility（服务端已通过 GAS 复制 AbilitySpec 到客户端）
	// 不 Spawn 武器 Actor（武器视觉本期仅服务端辅助枪口定位）
	if (InWeaponData)
	{
		CurrentWeaponData = InWeaponData;
		CurrentWeaponIndex = WeaponInventory.IndexOfByKey(InWeaponData);
	}
}

void UWeaponEquipmentComponent::UnequipWeapon()
{
	// 非服务端只清理本地数据引用
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		CurrentWeaponData = nullptr;
		CurrentWeaponActor = nullptr;
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

	CurrentWeaponData = nullptr;
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
	// 边界检查
	if (NewIndex < 0 || NewIndex >= WeaponInventory.Num())
	{
		return;
	}

	// 已经是当前武器，无需切换
	if (NewIndex == CurrentWeaponIndex)
	{
		return;
	}

	const bool bHasAuthority = GetOwner() && GetOwner()->HasAuthority();

	if (bHasAuthority)
	{
		// 服务端：完整卸载旧武器 + 装备新武器
		UnequipWeapon();
		CurrentWeaponIndex = NewIndex;
		EquipWeapon(WeaponInventory[NewIndex]);
	}
	else
	{
		// 客户端：仅清理本地数据引用 + 设置新武器数据
		UnequipWeapon();
		CurrentWeaponIndex = NewIndex;
		EquipWeaponLocal(WeaponInventory[NewIndex]);
	}
}

void UWeaponEquipmentComponent::SwitchToNext()
{
	if (WeaponInventory.Num() <= 1)
	{
		return;
	}

	const int32 NewIndex = (CurrentWeaponIndex + 1) % WeaponInventory.Num();
	SwitchWeapon(NewIndex);
}

void UWeaponEquipmentComponent::SwitchToPrevious()
{
	if (WeaponInventory.Num() <= 1)
	{
		return;
	}

	const int32 NewIndex = (CurrentWeaponIndex - 1 + WeaponInventory.Num()) % WeaponInventory.Num();
	SwitchWeapon(NewIndex);
}

void UWeaponEquipmentComponent::AddWeaponToInventory(UWeaponDataAsset* WeaponData)
{
	if (!WeaponData)
	{
		return;
	}

	// 不重复添加
	if (WeaponInventory.Contains(WeaponData))
	{
		return;
	}

	WeaponInventory.Add(WeaponData);

	// 如果是第一把武器，自动装备
	if (CurrentWeaponIndex == INDEX_NONE)
	{
		SwitchWeapon(0);
	}
}

void UWeaponEquipmentComponent::RemoveWeaponFromInventory(UWeaponDataAsset* WeaponData)
{
	if (!WeaponData)
	{
		return;
	}

	const int32 RemoveIndex = WeaponInventory.IndexOfByKey(WeaponData);
	if (RemoveIndex == INDEX_NONE)
	{
		return;
	}

	// 如果移除的是当前装备的武器
	if (RemoveIndex == CurrentWeaponIndex)
	{
		UnequipWeapon();
		WeaponInventory.RemoveAt(RemoveIndex);

		// 库存还有武器则切到第 0 个，否则没有武器
		if (WeaponInventory.Num() > 0)
		{
			SwitchWeapon(0);
		}
		else
		{
			CurrentWeaponIndex = INDEX_NONE;
		}
	}
	else
	{
		// 移除的不是当前装备的武器，只从数组移除
		WeaponInventory.RemoveAt(RemoveIndex);

		// 如果移除的武器索引在当前武器之前，需要前移索引
		if (RemoveIndex < CurrentWeaponIndex)
		{
			CurrentWeaponIndex--;
		}
	}
}
