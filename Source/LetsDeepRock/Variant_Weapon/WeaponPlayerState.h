// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "AbilitySystemInterface.h"
#include "WeaponPlayerState.generated.h"

class UAbilitySystemComponent;
class UWeaponHealthSet;

/**
 *  武器系统玩家状态
 *
 *  按 GAS 标准范式，玩家的 AbilitySystemComponent 挂在 PlayerState 上：
 *  - OwnerActor = 本 PlayerState
 *  - AvatarActor = 当前被 Possess 的 WeaponCharacter
 *  好处：属性/效果在 Pawn 重生间持久化，天然适配联机。
 *
 *  ASC 的 InitAbilityActorInfo 由 WeaponCharacter 在
 *  PossessedBy（服务端）/ OnRep_PlayerState（客户端）中调用。
 */
UCLASS()
class AWeaponPlayerState : public APlayerState, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:

	AWeaponPlayerState();

	//~ IAbilitySystemInterface
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

	/** 类型化访问 HealthSet */
	UWeaponHealthSet* GetHealthSet() const { return HealthSet; }

protected:

	/** 能力系统组件（在 PlayerState 上创建并复制） */
	UPROPERTY(VisibleAnywhere, Category = "GAS")
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	/** 生命值属性集 */
	UPROPERTY()
	TObjectPtr<UWeaponHealthSet> HealthSet;
};
