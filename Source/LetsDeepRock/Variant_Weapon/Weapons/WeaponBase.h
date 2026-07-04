// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "WeaponBase.generated.h"

class UWeaponDataAsset;
class UStaticMeshComponent;
class USceneComponent;

/**
 *  武器世界 Actor 基类
 *
 *  负责世界中的视觉表现（网格 + 枪口挂点）。
 *  由 WeaponEquipmentComponent 在装备武器时 Spawn 并 Attach 到角色。
 *  不包含任何 Ability/GAS 逻辑——那些由 GA 和 EquipmentComponent 管理。
 */
UCLASS()
class AWeaponBase : public AActor
{
	GENERATED_BODY()

public:

	AWeaponBase();

	/** 用 DataAsset 初始化武器外观 */
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	void InitFromDataAsset(const UWeaponDataAsset* InData);

	/** 获取枪口世界位置（供 GA_Fire 读取生成投射物的位置） */
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	FVector GetMuzzleLocation() const;

	/** 获取枪口朝向 */
	UFUNCTION(BlueprintCallable, Category = "Weapon")
	FRotator GetMuzzleRotation() const;

	/** 获取关联的 DataAsset */
	const UWeaponDataAsset* GetWeaponData() const { return WeaponData; }

protected:

	/** 场景根 */
	UPROPERTY(VisibleAnywhere, Category = "Components")
	TObjectPtr<USceneComponent> SceneRoot;

	/** 武器网格 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<UStaticMeshComponent> MeshComponent;

	/** 枪口挂点（相对于网格的位置，供投射物生成使用） */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components")
	TObjectPtr<USceneComponent> MuzzlePoint;

	/** 缓存的 DataAsset 引用 */
	UPROPERTY()
	TObjectPtr<const UWeaponDataAsset> WeaponData;
};
