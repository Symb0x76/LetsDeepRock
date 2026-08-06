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
#include "Net/UnrealNetwork.h"
#include "WeaponGameState.h"

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
	if (!GetOwner() || !GetOwner()->HasAuthority() || !InWeaponData)
	{
		return;
	}
	const int32 InventoryIndex = EquipmentState.WeaponInventory.IndexOfByKey(InWeaponData);
	if (InventoryIndex == INDEX_NONE)
	{
		return;
	}

	// 先卸载旧武器
	if (CurrentWeaponData)
	{
		UnequipWeapon();
	}

	CurrentWeaponData = InWeaponData;
	EquipmentState.CurrentWeaponIndex = InventoryIndex;
	InitializeFireModes();

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

	GetOwner()->ForceNetUpdate();
	NotifyEquipmentChanged();
}

void UWeaponEquipmentComponent::EquipWeaponLocal(UWeaponDataAsset* InWeaponData)
{
	// 保留旧 API，但装备状态只能由服务端修改；客户端等待 EquipmentState 复制。
	EquipWeapon(InWeaponData);
}

void UWeaponEquipmentComponent::OnRep_EquipmentState()
{
	UWeaponDataAsset* PreviousWeaponData = CurrentWeaponData;
	CurrentWeaponData = EquipmentState.WeaponInventory.IsValidIndex(EquipmentState.CurrentWeaponIndex)
		? EquipmentState.WeaponInventory[EquipmentState.CurrentWeaponIndex]
		: nullptr;
	if (CurrentWeaponData != PreviousWeaponData)
	{
		InitializeFireModes();
	}
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
	if (AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}
	SwitchWeaponAuthority(NewIndex);
}

void UWeaponEquipmentComponent::SwitchWeaponAuthority(const int32 NewIndex)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() ||
		AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}
	if (NewIndex == INDEX_NONE)
	{
		SelectNonWeaponSlot();
		return;
	}
	if (!EquipmentState.WeaponInventory.IsValidIndex(NewIndex) || NewIndex == EquipmentState.CurrentWeaponIndex)
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
	CancelReload();
	CurrentFireModeIds.Reset();
	CurrentFireModeId = NAME_None;

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

void UWeaponEquipmentComponent::InitializeFireModes()
{
	CurrentFireModeIds.Reset();
	CurrentFireModeId = NAME_None;
	if (!CurrentWeaponData || CurrentWeaponData->WeaponConfigId.IsNone())
	{
		return;
	}

	const UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	const UGameDataSubsystem* DataSubsystem = GameInstance ? GameInstance->GetSubsystem<UGameDataSubsystem>() : nullptr;
	const FWeaponConfigRow* Config = DataSubsystem ? DataSubsystem->GetWeaponConfig(CurrentWeaponData->WeaponConfigId) : nullptr;
	if (!Config)
	{
		return;
	}

	TArray<FString> ModeStrings;
	Config->FireModeIds.ParseIntoArray(ModeStrings, TEXT(","), true);
	for (FString& ModeString : ModeStrings)
	{
		ModeString.TrimStartAndEndInline();
		if (!ModeString.IsEmpty())
		{
			CurrentFireModeIds.AddUnique(FName(*ModeString));
		}
	}

	// 默认模式仅用于装备初始化/配置缺失回退，运行时读取统一走 CurrentFireModeId。
	if (CurrentFireModeIds.Num() == 0 && !Config->DefaultFireModeId.IsNone())
	{
		CurrentFireModeIds.Add(Config->DefaultFireModeId);
	}
	CurrentFireModeId = CurrentFireModeIds.Contains(Config->DefaultFireModeId)
		? Config->DefaultFireModeId
		: (CurrentFireModeIds.Num() > 0 ? CurrentFireModeIds[0] : NAME_None);
}

void UWeaponEquipmentComponent::CycleFireMode()
{
	if (CurrentFireModeIds.Num() <= 1)
	{
		return;
	}

	const int32 CurrentIndex = CurrentFireModeIds.IndexOfByKey(CurrentFireModeId);
	CurrentFireModeId = CurrentFireModeIds[(CurrentIndex + 1) % CurrentFireModeIds.Num()];
}

// ---- 多武器库存与切换实现 ----

void UWeaponEquipmentComponent::SwitchWeapon(int32 NewIndex)
{
	if (AWeaponGameState::IsGameplayRecoveryBlockedFor(this) ||
		!EquipmentState.WeaponInventory.IsValidIndex(NewIndex) ||
		NewIndex == EquipmentState.CurrentWeaponIndex || !GetOwner())
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
	return EquipmentState.WeaponAmmoStates.IsValidIndex(EquipmentState.CurrentWeaponIndex) ? EquipmentState.WeaponAmmoStates[EquipmentState.CurrentWeaponIndex].MagazineAmmo : 0;
}

int32 UWeaponEquipmentComponent::GetCurrentReserveAmmo() const
{
	return EquipmentState.WeaponAmmoStates.IsValidIndex(EquipmentState.CurrentWeaponIndex) ? EquipmentState.WeaponAmmoStates[EquipmentState.CurrentWeaponIndex].ReserveAmmo : 0;
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
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return 0.0f;
	}
	return EquipmentState.bIsReloading && GetWorld() ? GetWorld()->GetTimerManager().GetTimerRemaining(ReloadTimerHandle) : 0.0f;
}

void UWeaponEquipmentComponent::StartReload()
{
	if (!GetOwner() || !GetOwner()->HasAuthority() ||
		AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}

	if (EquipmentState.bIsReloading)
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] StartReload rejected: already reloading Weapon=%s"), *GetNameSafe(CurrentWeaponData));
		#endif
		return;
	}
	if (!EquipmentState.WeaponAmmoStates.IsValidIndex(EquipmentState.CurrentWeaponIndex))
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] StartReload rejected: invalid weapon index Index=%d Weapon=%s"), EquipmentState.CurrentWeaponIndex, *GetNameSafe(CurrentWeaponData));
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

	const FWeaponAmmoState& AmmoState = EquipmentState.WeaponAmmoStates[EquipmentState.CurrentWeaponIndex];
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

	EquipmentState.bIsReloading = true;
	GetOwner()->ForceNetUpdate();
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
	if (!EquipmentState.bIsReloading || !EquipmentState.WeaponAmmoStates.IsValidIndex(EquipmentState.CurrentWeaponIndex))
	{
		return;
	}

	int32 MagazineSize = 0;
	int32 InitialReserve = 0;
	float ReloadDuration = 0.0f;
	if (GetCurrentAmmoConfig(MagazineSize, InitialReserve, ReloadDuration))
	{
		FWeaponAmmoState& AmmoState = EquipmentState.WeaponAmmoStates[EquipmentState.CurrentWeaponIndex];
		const int32 AmmoToLoad = FMath::Min(MagazineSize - AmmoState.MagazineAmmo, AmmoState.ReserveAmmo);
		AmmoState.MagazineAmmo += AmmoToLoad;
		AmmoState.ReserveAmmo -= AmmoToLoad;
	}
	EquipmentState.bIsReloading = false;
	GetOwner()->ForceNetUpdate();
	#if !UE_BUILD_SHIPPING
	UE_LOG(LogTemp, Log, TEXT("[FireTrace] Reload completed Weapon=%s Magazine=%d Reserve=%d"),
		*GetNameSafe(CurrentWeaponData), GetCurrentMagazineAmmo(), GetCurrentReserveAmmo());
	#endif
}

void UWeaponEquipmentComponent::CancelReload()
{
	const bool bWasReloading = EquipmentState.bIsReloading;
	if (GetWorld())
	{
		GetWorld()->GetTimerManager().ClearTimer(ReloadTimerHandle);
	}
	EquipmentState.bIsReloading = false;
	if (bWasReloading && GetOwner())
	{
		GetOwner()->ForceNetUpdate();
	}
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
	if (!GetOwner() || !GetOwner()->HasAuthority() ||
		AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return false;
	}

	if (EquipmentState.bIsReloading || !EquipmentState.WeaponAmmoStates.IsValidIndex(EquipmentState.CurrentWeaponIndex))
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] ConsumeAmmo failed Weapon=%s Reloading=%s Index=%d"),
			*GetNameSafe(CurrentWeaponData), EquipmentState.bIsReloading ? TEXT("true") : TEXT("false"), EquipmentState.CurrentWeaponIndex);
		#endif
		return false;
	}

	FWeaponAmmoState& AmmoState = EquipmentState.WeaponAmmoStates[EquipmentState.CurrentWeaponIndex];
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
	GetOwner()->ForceNetUpdate();
	#if !UE_BUILD_SHIPPING
	UE_LOG(LogTemp, Log, TEXT("[FireTrace] ConsumeAmmo succeeded Weapon=%s Magazine=%d Reserve=%d Reloading=%s"),
		*GetNameSafe(CurrentWeaponData), AmmoState.MagazineAmmo, AmmoState.ReserveAmmo, EquipmentState.bIsReloading ? TEXT("true") : TEXT("false"));
	#endif
	return true;
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

	FWeaponAmmoState& AmmoState = EquipmentState.WeaponAmmoStates.AddDefaulted_GetRef();
	const int32 AddedIndex = EquipmentState.WeaponInventory.Num() - 1;
	const int32 PreviousIndex = EquipmentState.CurrentWeaponIndex;
	UWeaponDataAsset* PreviousWeapon = CurrentWeaponData;
	EquipmentState.CurrentWeaponIndex = AddedIndex;
	CurrentWeaponData = WeaponData;
	int32 MagazineSize = 0;
	float ReloadDuration = 0.0f;
	GetCurrentAmmoConfig(MagazineSize, AmmoState.ReserveAmmo, ReloadDuration);
	AmmoState.MagazineAmmo = MagazineSize;
	EquipmentState.CurrentWeaponIndex = PreviousIndex;
	CurrentWeaponData = PreviousWeapon;
	if (EquipmentState.WeaponInventory.IsValidIndex(EquipmentState.CurrentWeaponIndex))
	{
		CurrentWeaponData = EquipmentState.WeaponInventory[EquipmentState.CurrentWeaponIndex];
	}

	// 如果是第一把武器，自动装备
	if (EquipmentState.CurrentWeaponIndex == INDEX_NONE)
	{
		// This is authority-owned initialization, not a player switching intent.
		// SwitchWeapon() is correctly rejected by the recovery barrier, but that
		// used to leave reconnecting Characters with inventory and no equipped
		// weapon/Fire Ability. Equip directly; the Character barrier still blocks
		// Ability activation until RecoveryReady is complete.
		EquipWeapon(EquipmentState.WeaponInventory[0]);
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
		EquipmentState.WeaponAmmoStates.RemoveAt(RemoveIndex);
		EquipmentState.CurrentWeaponIndex = INDEX_NONE;

		// 库存还有武器则切到第 0 个，否则没有武器
		if (EquipmentState.WeaponInventory.Num() > 0)
		{
			SwitchWeapon(0);
		}
		else
		{
			EquipmentState.CurrentWeaponIndex = INDEX_NONE;
		}
	}
	else
	{
		// 移除的不是当前装备的武器，只从数组移除
		EquipmentState.WeaponInventory.RemoveAt(RemoveIndex);
		EquipmentState.WeaponAmmoStates.RemoveAt(RemoveIndex);

		// 如果移除的武器索引在当前武器之前，需要前移索引
		if (RemoveIndex < EquipmentState.CurrentWeaponIndex)
		{
			EquipmentState.CurrentWeaponIndex--;
		}
	}
}

void UWeaponEquipmentComponent::SelectNonWeaponSlot()
{
	if (!GetOwner())
	{
		return;
	}
	if (!GetOwner()->HasAuthority())
	{
		ServerSwitchWeapon(INDEX_NONE);
		return;
	}

	UnequipWeapon();
	EquipmentState.CurrentWeaponIndex = INDEX_NONE;
	CurrentWeaponData = nullptr;
	GetOwner()->ForceNetUpdate();
	NotifyEquipmentChanged();
}

bool UWeaponEquipmentComponent::RestoreWeaponSelection(const int32 SavedWeaponIndex)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() ||
		(SavedWeaponIndex != INDEX_NONE && !EquipmentState.WeaponInventory.IsValidIndex(SavedWeaponIndex)))
	{
		return false;
	}
	UnequipWeapon();
	EquipmentState.CurrentWeaponIndex = INDEX_NONE;
	CurrentWeaponData = nullptr;

	if (SavedWeaponIndex != INDEX_NONE)
	{
		EquipWeapon(EquipmentState.WeaponInventory[SavedWeaponIndex]);
	}
	else
	{
		GetOwner()->ForceNetUpdate();
		NotifyEquipmentChanged();
	}
	return true;
}

void UWeaponEquipmentComponent::CaptureSnapshotState(FWeaponEquipmentSnapshotState& OutState) const
{
	OutState.WeaponInventory = EquipmentState.WeaponInventory;
	OutState.WeaponAmmoStates = EquipmentState.WeaponAmmoStates;
	OutState.CurrentWeaponIndex = EquipmentState.CurrentWeaponIndex;
	OutState.bWasReloading = EquipmentState.bIsReloading;
}

bool UWeaponEquipmentComponent::RestoreSnapshotState(const FWeaponEquipmentSnapshotState& InState)
{
	if (!GetOwner() || !GetOwner()->HasAuthority() ||
		InState.WeaponInventory.Num() != InState.WeaponAmmoStates.Num() ||
		(InState.CurrentWeaponIndex != INDEX_NONE && !InState.WeaponInventory.IsValidIndex(InState.CurrentWeaponIndex)))
	{
		return false;
	}

	UnequipWeapon();
	EquipmentState.WeaponInventory = InState.WeaponInventory;
	EquipmentState.WeaponAmmoStates = InState.WeaponAmmoStates;
	EquipmentState.CurrentWeaponIndex = INDEX_NONE;
	EquipmentState.bIsReloading = false; // 快照不恢复计时器，避免无可靠剩余时间时卡死。
	CurrentWeaponData = nullptr;
	if (InState.CurrentWeaponIndex != INDEX_NONE)
	{
		EquipWeapon(EquipmentState.WeaponInventory[InState.CurrentWeaponIndex]);
	}
	else
	{
		GetOwner()->ForceNetUpdate();
		NotifyEquipmentChanged();
	}
	return true;
}
