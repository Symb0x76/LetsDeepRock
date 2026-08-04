// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponEquipmentComponent.h"
#include "WeaponDataAsset.h"
#include "WeaponBase.h"
#include "AbilitySystemComponent.h"
#include "AbilitySystemInterface.h"
#include "GameFramework/Character.h"
#include "Data/GameDataSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "TimerManager.h"

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

UWeaponDataAsset* UWeaponEquipmentComponent::ResolveWeaponData(FName WeaponId) const
{
	const TCHAR* ObjectPath = nullptr;
	if (WeaponId == FName(TEXT("Ranger_ArcaneCrossbow")))
	{
		ObjectPath = TEXT("/Game/Variant_Weapon/Weapons/DA_Ranger_ArcaneCrossbow.DA_Ranger_ArcaneCrossbow");
	}
	else if (WeaponId == FName(TEXT("Ranger_SawedOffShotgun")))
	{
		ObjectPath = TEXT("/Game/Variant_Weapon/Weapons/DA_Ranger_SawedOffShotgun.DA_Ranger_SawedOffShotgun");
	}
	else if (WeaponId == FName(TEXT("Ranger_FullAutoPistol")))
	{
		ObjectPath = TEXT("/Game/Variant_Weapon/Weapons/DA_Ranger_FullAutoPistol.DA_Ranger_FullAutoPistol");
	}
	else if (WeaponId == FName(TEXT("Ranger_ThrowingKnife")))
	{
		ObjectPath = TEXT("/Game/Variant_Weapon/Weapons/DA_Ranger_ThrowingKnife.DA_Ranger_ThrowingKnife");
	}
	else if (WeaponId == FName(TEXT("Demolitionist_Flamethrower")))
	{
		ObjectPath = TEXT("/Game/Variant_Weapon/Weapons/DA_Demolitionist_Flamethrower.DA_Demolitionist_Flamethrower");
	}
	else if (WeaponId == FName(TEXT("Demolitionist_Revolver")))
	{
		ObjectPath = TEXT("/Game/Variant_Weapon/Weapons/DA_Demolitionist_Revolver.DA_Demolitionist_Revolver");
	}
	else if (WeaponId == FName(TEXT("Demolitionist_RocketLauncher")))
	{
		ObjectPath = TEXT("/Game/Variant_Weapon/Weapons/DA_Demolitionist_RocketLauncher.DA_Demolitionist_RocketLauncher");
	}
	else if (WeaponId == FName(TEXT("Engineer_AssaultRifle")))
	{
		ObjectPath = TEXT("/Game/Variant_Weapon/Weapons/DA_Engineer_AssaultRifle.DA_Engineer_AssaultRifle");
	}
	else if (WeaponId == FName(TEXT("Engineer_ChainsawLauncher")))
	{
		ObjectPath = TEXT("/Game/Variant_Weapon/Weapons/DA_Engineer_ChainsawLauncher.DA_Engineer_ChainsawLauncher");
	}

	return ObjectPath ? LoadObject<UWeaponDataAsset>(nullptr, ObjectPath) : nullptr;
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

	// 1. 授予射击 Ability。阶段1保留 DataAsset 强类型类引用；表内字符串类路径不做猜测式加载。
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
	// 阶段1保留 DataAsset 的 WeaponMesh；表内字符串可能是包路径或对象路径，不做猜测式同步加载。
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
	CancelReload();

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

bool UWeaponEquipmentComponent::GetCurrentAmmoConfig(int32& OutMagazineSize, int32& OutReserveAmmo, float& OutReloadDuration) const
{
	OutMagazineSize = 0;
	OutReserveAmmo = 0;
	OutReloadDuration = 0.0f;
	if (!CurrentWeaponData || CurrentWeaponData->WeaponConfigId.IsNone())
	{
		return false;
	}

	UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	const UGameDataSubsystem* DataSubsystem = GameInstance ? GameInstance->GetSubsystem<UGameDataSubsystem>() : nullptr;
	const FWeaponConfigRow* Config = DataSubsystem ? DataSubsystem->GetWeaponConfig(CurrentWeaponData->WeaponConfigId) : nullptr;
	if (!Config)
	{
		return false;
	}

	OutMagazineSize = FMath::Max(Config->MagazineSize, 0);
	OutReserveAmmo = FMath::Max(Config->ReserveAmmo, 0);
	OutReloadDuration = FMath::Max(Config->ReloadDuration, 0.0f);
	return true;
}

int32 UWeaponEquipmentComponent::GetCurrentMagazineAmmo() const
{
	return WeaponAmmoStates.IsValidIndex(CurrentWeaponIndex) ? WeaponAmmoStates[CurrentWeaponIndex].MagazineAmmo : 0;
}

int32 UWeaponEquipmentComponent::GetCurrentReserveAmmo() const
{
	return WeaponAmmoStates.IsValidIndex(CurrentWeaponIndex) ? WeaponAmmoStates[CurrentWeaponIndex].ReserveAmmo : 0;
}

int32 UWeaponEquipmentComponent::GetCurrentMagazineSize() const
{
	int32 MagazineSize = 0;
	int32 InitialReserve = 0;
	float ReloadDuration = 0.0f;
	GetCurrentAmmoConfig(MagazineSize, InitialReserve, ReloadDuration);
	return MagazineSize;
}

float UWeaponEquipmentComponent::GetReloadTimeRemaining() const
{
	return bIsReloading && GetWorld() ? GetWorld()->GetTimerManager().GetTimerRemaining(ReloadTimerHandle) : 0.0f;
}

void UWeaponEquipmentComponent::StartReload()
{
	if (bIsReloading)
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] StartReload rejected: already reloading Weapon=%s"), *GetNameSafe(CurrentWeaponData));
		#endif
		return;
	}
	if (!WeaponAmmoStates.IsValidIndex(CurrentWeaponIndex))
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] StartReload rejected: invalid weapon index Index=%d Weapon=%s"), CurrentWeaponIndex, *GetNameSafe(CurrentWeaponData));
		#endif
		return;
	}

	int32 MagazineSize = 0;
	int32 InitialReserve = 0;
	float ReloadDuration = 0.0f;
	if (!GetCurrentAmmoConfig(MagazineSize, InitialReserve, ReloadDuration))
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] StartReload rejected: ammo config missing Weapon=%s WeaponConfigId=%s"),
			*GetNameSafe(CurrentWeaponData), CurrentWeaponData ? *CurrentWeaponData->WeaponConfigId.ToString() : TEXT("None"));
		#endif
		return;
	}

	const FWeaponAmmoState& AmmoState = WeaponAmmoStates[CurrentWeaponIndex];
	if (AmmoState.MagazineAmmo >= MagazineSize)
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] StartReload rejected: magazine full Weapon=%s Magazine=%d/%d Reserve=%d"),
			*GetNameSafe(CurrentWeaponData), AmmoState.MagazineAmmo, MagazineSize, AmmoState.ReserveAmmo);
		#endif
		return;
	}
	if (AmmoState.ReserveAmmo <= 0)
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] StartReload rejected: no reserve ammo Weapon=%s Magazine=%d/%d Reserve=%d"),
			*GetNameSafe(CurrentWeaponData), AmmoState.MagazineAmmo, MagazineSize, AmmoState.ReserveAmmo);
		#endif
		return;
	}

	bIsReloading = true;
	#if !UE_BUILD_SHIPPING
	UE_LOG(LogTemp, Log, TEXT("[FireTrace] Reload started Weapon=%s Magazine=%d/%d Reserve=%d Duration=%.3f"),
		*GetNameSafe(CurrentWeaponData), AmmoState.MagazineAmmo, MagazineSize, AmmoState.ReserveAmmo, ReloadDuration);
	#endif
	if (ReloadDuration <= 0.0f)
	{
		FinishReload();
		return;
	}

	GetWorld()->GetTimerManager().SetTimer(ReloadTimerHandle, this, &UWeaponEquipmentComponent::FinishReload, ReloadDuration, false);
}

void UWeaponEquipmentComponent::FinishReload()
{
	if (!bIsReloading || !WeaponAmmoStates.IsValidIndex(CurrentWeaponIndex))
	{
		return;
	}

	int32 MagazineSize = 0;
	int32 InitialReserve = 0;
	float ReloadDuration = 0.0f;
	if (GetCurrentAmmoConfig(MagazineSize, InitialReserve, ReloadDuration))
	{
		FWeaponAmmoState& AmmoState = WeaponAmmoStates[CurrentWeaponIndex];
		const int32 AmmoToLoad = FMath::Min(MagazineSize - AmmoState.MagazineAmmo, AmmoState.ReserveAmmo);
		AmmoState.MagazineAmmo += AmmoToLoad;
		AmmoState.ReserveAmmo -= AmmoToLoad;
	}
	bIsReloading = false;
	#if !UE_BUILD_SHIPPING
	UE_LOG(LogTemp, Log, TEXT("[FireTrace] Reload completed Weapon=%s Magazine=%d Reserve=%d"),
		*GetNameSafe(CurrentWeaponData), GetCurrentMagazineAmmo(), GetCurrentReserveAmmo());
	#endif
}

void UWeaponEquipmentComponent::CancelReload()
{
	const bool bWasReloading = bIsReloading;
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(ReloadTimerHandle);
	}
	bIsReloading = false;
	#if !UE_BUILD_SHIPPING
	if (bWasReloading)
	{
		UE_LOG(LogTemp, Log, TEXT("[FireTrace] Reload canceled Weapon=%s Magazine=%d Reserve=%d"),
			*GetNameSafe(CurrentWeaponData), GetCurrentMagazineAmmo(), GetCurrentReserveAmmo());
	}
	#endif
}

bool UWeaponEquipmentComponent::ConsumeAmmoForShot()
{
	if (bIsReloading || !WeaponAmmoStates.IsValidIndex(CurrentWeaponIndex))
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] ConsumeAmmo failed Weapon=%s Reloading=%s Index=%d"),
			*GetNameSafe(CurrentWeaponData), bIsReloading ? TEXT("true") : TEXT("false"), CurrentWeaponIndex);
		#endif
		return false;
	}

	FWeaponAmmoState& AmmoState = WeaponAmmoStates[CurrentWeaponIndex];
	if (AmmoState.MagazineAmmo <= 0)
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] ConsumeAmmo failed: empty magazine Weapon=%s Reserve=%d"),
			*GetNameSafe(CurrentWeaponData), AmmoState.ReserveAmmo);
		#endif
		StartReload();
		return false;
	}

	--AmmoState.MagazineAmmo;
	if (AmmoState.MagazineAmmo == 0)
	{
		StartReload();
	}
	#if !UE_BUILD_SHIPPING
	UE_LOG(LogTemp, Log, TEXT("[FireTrace] ConsumeAmmo succeeded Weapon=%s Magazine=%d Reserve=%d Reloading=%s"),
		*GetNameSafe(CurrentWeaponData), AmmoState.MagazineAmmo, AmmoState.ReserveAmmo, bIsReloading ? TEXT("true") : TEXT("false"));
	#endif
	return true;
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

	FWeaponAmmoState& AmmoState = WeaponAmmoStates.AddDefaulted_GetRef();
	const int32 AddedIndex = WeaponInventory.Num() - 1;
	const int32 PreviousIndex = CurrentWeaponIndex;
	UWeaponDataAsset* PreviousWeapon = CurrentWeaponData;
	CurrentWeaponIndex = AddedIndex;
	CurrentWeaponData = WeaponData;
	int32 MagazineSize = 0;
	float ReloadDuration = 0.0f;
	GetCurrentAmmoConfig(MagazineSize, AmmoState.ReserveAmmo, ReloadDuration);
	AmmoState.MagazineAmmo = MagazineSize;
	CurrentWeaponIndex = PreviousIndex;
	CurrentWeaponData = PreviousWeapon;

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
		WeaponAmmoStates.RemoveAt(RemoveIndex);
		CurrentWeaponIndex = INDEX_NONE;

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
		WeaponAmmoStates.RemoveAt(RemoveIndex);

		// 如果移除的武器索引在当前武器之前，需要前移索引
		if (RemoveIndex < CurrentWeaponIndex)
		{
			CurrentWeaponIndex--;
		}
	}
}
