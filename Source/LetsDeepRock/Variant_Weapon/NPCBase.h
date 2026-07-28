// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "Abilities/GameplayAbility.h"
#include "NPCBase.generated.h"

class UAbilitySystemComponent;
class UWeaponHealthSet;
class UWeaponResistanceSet;
class UWeaponMovementSet;

/**
 *  GAS 自持 NPC 基类
 *
 *  ASC 直接挂在 Pawn 上（OwnerActor = AvatarActor = 本 NPC 自己），
 *  适用于不需要 PlayerState 的 AI 角色（敌人、友方 NPC、召唤物等）。
 *
 *  子类通过覆写 BeginPlay 添加专属配置（如 DataTable 初始化），
 *  通过设置 AIControllerClass 选择 AI 方案。
 */
UCLASS()
class ANPCBase : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:

	ANPCBase();

	//~ IAbilitySystemInterface
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

	UWeaponHealthSet* GetHealthSet() const { return HealthSet; }


protected:

	virtual void BeginPlay() override;

	/** 能力系统组件（NPC 自持） */
	UPROPERTY(VisibleAnywhere, Category = "GAS")
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	/** 生命值属性集 */
	UPROPERTY()
	TObjectPtr<UWeaponHealthSet> HealthSet;

	/** 抗性属性集 */
	UPROPERTY()
	TObjectPtr<UWeaponResistanceSet> ResistanceSet;

	/** 移动属性集 */
	UPROPERTY()
	TObjectPtr<UWeaponMovementSet> MovementSet;

	/** 初始生命值（无 DataTable 配置时作为 fallback） */
	UPROPERTY(EditAnywhere, Category = "Attributes", meta = (ClampMin = 1))
	float DefaultHealth = 30.0f;

	/** 死亡后延迟销毁时间（秒） */
	UPROPERTY(EditAnywhere, Category = "Death", meta = (ClampMin = 0))
	float DeathLingerTime = 2.0f;

	/** 默认拥有的能力列表（BeginPlay 时自动 Grant） */
	UPROPERTY(EditAnywhere, Category = "GAS|Abilities")
	TArray<TSubclassOf<UGameplayAbility>> DefaultAbilities;

	/** Health 归零时的回调（子类可覆写添加额外逻辑） */
	virtual void HandleHealthDepleted(AActor* OwningActor);
};
