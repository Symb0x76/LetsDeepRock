// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayAbilitySpec.h"
#include "WeaponDataAsset.h"
#include "WeaponEquipmentComponent.generated.h"

class AWeaponBase;
class UAbilitySystemComponent;

USTRUCT(BlueprintType)
struct FWeaponAmmoState
{
	GENERATED_BODY()

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon|Ammo")
	int32 MagazineAmmo = 0;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Weapon|Ammo")
	int32 ReserveAmmo = 0;
};

/**
 *  武器装备管理组件
 *
 *  挂在 WeaponCharacter 上，负责：
 *  1. 根据 WeaponDataAsset 装备武器（Spawn WeaponBase Actor + Attach）
 *  2. 向 ASC 授予射击 Ability（GiveAbility）并保存 SpecHandle
 *  3. 切换武器时清除旧 Ability（ClearAbility）并卸载旧武器 Actor
 *
 *  职责分离：本组件只管"装备/卸载"，不管射击逻辑（那是 GA_Fire 的事）。
 */
UCLASS(ClassGroup = (Weapon), meta = (BlueprintSpawnableComponent))
class UWeaponEquipmentComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UWeaponEquipmentComponent();

	/** 装备指定武器（可在 BeginPlay 或运行时切枪调用） */
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	void EquipWeapon(UWeaponDataAsset* InWeaponData);

	/** 客户端仅设置武器数据引用（不 GiveAbility，不 Spawn Actor） */
	void EquipWeaponLocal(UWeaponDataAsset* InWeaponData);

	/** 卸载当前武器 */
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	void UnequipWeapon();

	/** 获取当前武器数据（GA_Fire 读取伤害/投射物配置用） */
	UWeaponDataAsset* GetCurrentWeaponData() const { return CurrentWeaponData; }

	/** 获取当前武器 Actor（GA_Fire 读取枪口位置用） */
	AWeaponBase* GetCurrentWeaponActor() const { return CurrentWeaponActor; }

	/** 获取默认武器配置（供 WeaponCharacter::PossessedBy 调用） */
	UWeaponDataAsset* GetDefaultWeaponData() const { return DefaultWeapons.Num() > 0 ? DefaultWeapons[0] : nullptr; }

	/** 获取默认武器列表（供 WeaponCharacter 初始化库存用） */
	const TArray<UWeaponDataAsset*>& GetDefaultWeapons() const { return DefaultWeapons; }

	/** 将当前职业配置使用的武器 ID 显式解析为数据资产 */
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	UWeaponDataAsset* ResolveWeaponData(FName WeaponId) const;

	// ---- 多武器库存与切换 ----

	/** 切换到指定索引的武器（0-based） */
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	void SwitchWeapon(int32 NewIndex);

	/** 切换到下一把武器（循环） */
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	void SwitchToNext();

	/** 切换到上一把武器（循环） */
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	void SwitchToPrevious();

	/** 获取武器库存 */
	const TArray<TObjectPtr<UWeaponDataAsset>>& GetWeaponInventory() const { return WeaponInventory; }

	/** 获取当前武器索引 */
	int32 GetCurrentWeaponIndex() const { return CurrentWeaponIndex; }

	/** 当前弹匣内弹药数。 */
	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetCurrentMagazineAmmo() const;

	/** 当前武器备弹数。 */
	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetCurrentReserveAmmo() const;

	/** 当前武器弹匣容量。 */
	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	int32 GetCurrentMagazineSize() const;

	/** 当前武器是否正在换弹。 */
	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	bool IsReloading() const { return bIsReloading; }

	/** 当前换弹计时剩余秒数。 */
	UFUNCTION(BlueprintPure, Category = "Weapon|Ammo")
	float GetReloadTimeRemaining() const;

	/** 手动开始当前武器换弹；条件不满足时无操作。 */
	UFUNCTION(BlueprintCallable, Category = "Weapon|Ammo")
	void StartReload();

	/** GA_Fire 在 Commit 成功后调用；每次射击只消耗一发。 */
	bool ConsumeAmmoForShot();

	/** 向库存添加一把武器（如果已存在则不重复添加） */
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	void AddWeaponToInventory(UWeaponDataAsset* WeaponData);

	/** 从库存移除一把武器（如果是当前装备则自动切换） */
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	void RemoveWeaponFromInventory(UWeaponDataAsset* WeaponData);

protected:

	virtual void BeginPlay() override;

	/** 默认武器列表（编辑器中配置，PossessedBy 时加入库存并装备第 0 个） */
	UPROPERTY(EditAnywhere, Category = "Weapon")
	TArray<UWeaponDataAsset*> DefaultWeapons;

	/** 武器 Actor 挂载的 Socket 名称（在角色骨骼网格上）。TODO: 当前俯视角无骨骼，AttachToActor 未使用此 Socket */
	UPROPERTY(EditAnywhere, Category = "Weapon")
	FName AttachSocketName = TEXT("hand_r");

private:

	/** 获取 Owner 的 ASC */
	UAbilitySystemComponent* GetOwnerASC() const;

	/** 从 DT_WeaponConfig 读取当前武器弹药参数。 */
	bool GetCurrentAmmoConfig(int32& OutMagazineSize, int32& OutReserveAmmo, float& OutReloadDuration) const;

	/** 完成当前武器换弹。 */
	void FinishReload();

	/** 切换/卸载时取消旧武器换弹。 */
	void CancelReload();

	/** 当前武器数据引用 */
	UPROPERTY()
	UWeaponDataAsset* CurrentWeaponData;

	/** 当前武器世界 Actor */
	UPROPERTY()
	TObjectPtr<AWeaponBase> CurrentWeaponActor;

	/** 已授予的射击 Ability Handle（切枪时需要移除） */
	FGameplayAbilitySpecHandle FireAbilityHandle;

	/** 已授予的近战 Ability Handle */
	FGameplayAbilitySpecHandle MeleeAbilityHandle;

	/** 武器库存（所有可用武器） */
	UPROPERTY()
	TArray<TObjectPtr<UWeaponDataAsset>> WeaponInventory;

	/** 与 WeaponInventory 同索引的独立弹药状态。 */
	UPROPERTY()
	TArray<FWeaponAmmoState> WeaponAmmoStates;

	/** 当前装备的武器在库存中的索引 */
	int32 CurrentWeaponIndex = INDEX_NONE;

	bool bIsReloading = false;
	FTimerHandle ReloadTimerHandle;
};
