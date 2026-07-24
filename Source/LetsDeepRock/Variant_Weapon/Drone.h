// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "AbilitySystemInterface.h"
#include "Abilities/GameplayAbility.h"
#include "Drone.generated.h"

class UAbilitySystemComponent;
class UWeaponHealthSet;
struct FDroneConfigRow;

/**
 *  无人机（可召唤的轨道飞行单位）
 *
 *  APawn 而非 ACharacter：无人机通过 Tick 直接控制轨道运动，
 *  不需要 UCharacterMovementComponent 或胶囊体行走碰撞。
 *
 *  生命周期：Duration 倒计时结束 或 Health 归零 → 销毁。
 */
UCLASS()
class ADrone : public APawn, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:

	ADrone();

	//~ IAbilitySystemInterface
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

	UWeaponHealthSet* GetHealthSet() const { return HealthSet; }

	/** 设置轨道目标（由召唤者调用） */
	void SetOrbitTarget(AActor* Target);

protected:

	virtual void BeginPlay() override;
	virtual void Tick(float DeltaTime) override;

	/** 能力系统组件 */
	UPROPERTY(VisibleAnywhere, Category = "GAS")
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	/** 生命值属性集 */
	UPROPERTY()
	TObjectPtr<UWeaponHealthSet> HealthSet;

	/** 配置表行名（对应 DT_DroneConfig 中的 RowName） */
	UPROPERTY(EditAnywhere, Category = "Config")
	FName DroneConfigRowName = NAME_None;

	/** 默认拥有的能力列表（BeginPlay 时自动 Grant） */
	UPROPERTY(EditAnywhere, Category = "GAS|Abilities")
	TArray<TSubclassOf<UGameplayAbility>> DefaultAbilities;

	/** 轨道目标（通常为召唤者） */
	UPROPERTY(BlueprintReadOnly, Category = "Movement")
	TObjectPtr<AActor> OrbitTarget;

	/** 当前轨道角度（度） */
	float OrbitAngle = 0.0f;

	/** 剩余存活时间（秒） */
	float RemainingLifetime = 0.0f;

private:

	void InitializeFromConfig(const FDroneConfigRow& Config);
	void HandleHealthDepleted(AActor* OwningActor);
	void HandleLifetimeExpired();
};
