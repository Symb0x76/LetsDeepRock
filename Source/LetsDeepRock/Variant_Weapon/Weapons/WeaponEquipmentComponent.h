// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayAbilitySpec.h"
#include "WeaponDataAsset.h"
#include "WeaponEquipmentComponent.generated.h"

class AWeaponBase;
class UAbilitySystemComponent;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnWeaponEquipmentChanged, int32, NewWeaponIndex);

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

	virtual void BeginPlay() override;

	/** 服务端装备指定武器；客户端由复制结果更新状态。 */
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	void EquipWeapon(UWeaponDataAsset* InWeaponData);

	/** 卸载当前武器 */
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	void UnequipWeapon();

	/** 获取当前武器数据（GA_Fire 读取伤害/投射物配置用） */
	UWeaponDataAsset* GetCurrentWeaponData() const;

	/** 获取当前武器 Actor（GA_Fire 读取枪口位置用） */
	AWeaponBase* GetCurrentWeaponActor() const { return CurrentWeaponActor; }

	/** 获取默认武器配置（供 WeaponCharacter::PossessedBy 调用） */
	UWeaponDataAsset* GetDefaultWeaponData() const { return DefaultWeapons.Num() > 0 ? DefaultWeapons[0] : nullptr; }

	// ---- 固定武器表与选择 ----

	/** 切换到指定索引的武器（0-based） */
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	void SwitchWeapon(int32 NewIndex);

	/** 切换到下一把武器（循环） */
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	void SwitchToNext();

	/** 切换到上一把武器（循环） */
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	void SwitchToPrevious();

	/** 获取当前武器索引 */
	int32 GetCurrentWeaponIndex() const { return CurrentWeaponIndex; }

	/** 只读检查静态 DefaultWeapons 是否可以恢复指定选择。 */
	bool CanRestoreWeaponSelection(int32 SavedWeaponIndex) const;

	/** 在 CanRestoreWeaponSelection 成功后执行无业务失败的服务端 Apply。 */
	void RestoreWeaponSelection(int32 SavedWeaponIndex);

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 客户端收到服务端装备状态后通知 UI/表现层。 */
	UPROPERTY(BlueprintAssignable, Category = "Weapon")
	FOnWeaponEquipmentChanged OnEquipmentChanged;

protected:

	/** 默认武器列表（编辑器中配置，PossessedBy 时加入库存并装备第 0 个） */
	UPROPERTY(EditAnywhere, Category = "Weapon")
	TArray<UWeaponDataAsset*> DefaultWeapons;

	/** 武器 Actor 挂载的 Socket 名称（在角色骨骼网格上）。TODO: 当前俯视角无骨骼，AttachToActor 未使用此 Socket */
	UPROPERTY(EditAnywhere, Category = "Weapon")
	FName AttachSocketName = TEXT("hand_r");

private:
	/** 客户端的切枪意图；服务器会再次校验索引和库存。 */
	UFUNCTION(Server, Reliable)
	void ServerSwitchWeapon(int32 NewIndex);

	/** 只在服务器执行真正的卸载、Ability 更新和 Actor Spawn。 */
	void SwitchWeaponAuthority(int32 NewIndex);

	/** 当前复制状态到达客户端时调用。 */
	UFUNCTION()
	void OnRep_CurrentWeaponIndex();

	/** 服务端不会触发 OnRep，因此状态改变后主动通知本地 UI。 */
	void NotifyEquipmentChanged();

	/** 获取 Owner 的 ASC */
	UAbilitySystemComponent* GetOwnerASC() const;

	/** 当前武器世界 Actor */
	UPROPERTY()
	TObjectPtr<AWeaponBase> CurrentWeaponActor;

	/** 已授予的射击 Ability Handle（切枪时需要移除） */
	FGameplayAbilitySpecHandle FireAbilityHandle;

	/** 已授予的近战 Ability Handle */
	FGameplayAbilitySpecHandle MeleeAbilityHandle;

	/** DefaultWeapons 是静态配置；只有选择索引属于运行时复制状态。 */
	UPROPERTY(ReplicatedUsing = OnRep_CurrentWeaponIndex)
	int32 CurrentWeaponIndex = INDEX_NONE;
};
