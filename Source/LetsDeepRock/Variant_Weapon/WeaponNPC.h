// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "Abilities/GameplayAbility.h"
#include "WeaponNPC.generated.h"

class AEnemyAIController;

class UAbilitySystemComponent;
class UWeaponHealthSet;
class UWeaponResistanceSet;
class UWeaponMovementSet;
class UWeaponStableIdentityComponent;

/**
 *  武器系统敌人 NPC
 *
 *  与玩家不同，NPC 没有 PlayerState，因此 ASC 直接挂在 Pawn 上：
 *  OwnerActor = AvatarActor = 本 NPC 自己。
 *  这是玩家(ASC@PlayerState) + NPC(ASC@Pawn) 的标准混合模式。
 *
 *  伤害入口与玩家完全统一：外部只需通过 IAbilitySystemInterface
 *  拿到 ASC 施加 GE_Damage，不需要关心 ASC 挂在哪。
 */
UCLASS()
class AWeaponNPC : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:

	AWeaponNPC();

	//~ IAbilitySystemInterface
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

	UWeaponHealthSet* GetHealthSet() const { return HealthSet; }
	UWeaponStableIdentityComponent* GetStableIdentityComponent() const { return StableIdentityComponent; }

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

	/** 服务器分配的 NPC 稳定身份，跨 World 恢复时作为 FWeaponEnemySnapshot 的外键。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Snapshot|Identity")
	TObjectPtr<UWeaponStableIdentityComponent> StableIdentityComponent;

	/** 配置表行名（对应 DT_EnemyConfig 中的 RowName） */
	UPROPERTY(EditAnywhere, Category = "DataTable")
	FName EnemyConfigRowName = NAME_None;

	/** 初始生命值（BeginPlay 时写入 Health/MaxHealth） */
	UPROPERTY(EditAnywhere, Category = "Attributes", meta = (ClampMin = 1))
	float DefaultHealth = 30.0f;

	/** 死亡后延迟销毁时间（秒），给死亡特效/动画预留时间 */
	UPROPERTY(EditAnywhere, Category = "Death", meta = (ClampMin = 0))
	float DeathLingerTime = 2.0f;

	/** NPC 默认拥有的能力列表（BeginPlay 时自动 Grant） */
	UPROPERTY(EditAnywhere, Category = "GAS|Abilities")
	TArray<TSubclassOf<UGameplayAbility>> DefaultAbilities;

	/** Health 归零时的服务端回调 */
	virtual void HandleHealthDepleted(AActor* OwningActor);
};
