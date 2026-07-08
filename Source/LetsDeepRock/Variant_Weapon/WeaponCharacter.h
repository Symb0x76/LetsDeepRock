// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "WeaponCharacter.generated.h"

class USpringArmComponent;
class UCameraComponent;
struct FInputActionValue;
class UInputAction;
class UInputMappingContext;
class UAbilitySystemComponent;
class UWeaponEquipmentComponent;

/**
 *  俯视角武器系统角色（Variant_Weapon 骨架）
 *
 *  阶段 0：仅提供俯视角相机 + WASD 移动的最小可玩骨架。
 *  阶段 1 起将挂接 GAS：本 Character 作为 AvatarActor，ASC 挂在 PlayerState 上，
 *  并在 PossessedBy（服务端）/ OnRep_PlayerState（客户端）中调用 InitAbilityActorInfo。
 *  下方已预留这两个覆写点的占位（阶段 1 填充实现）。
 *
 *  3C 优化：角色始终面向鼠标位置（Tick 中射线检测鼠标与地面交点），
 *  移动方向与面向解耦，射击方向跟随角色面向。
 */
UCLASS(abstract)
class AWeaponCharacter : public ACharacter, public IAbilitySystemInterface
{
	GENERATED_BODY()

	/** 相机弹簧臂 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	USpringArmComponent* SpringArm;

	/** 玩家相机 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	UCameraComponent* Camera;

protected:

	/** 移动输入 Action */
	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* MoveAction;

	/** 射击输入 Action（左键/右扳机） */
	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* FireAction;

	/** 近战输入 Action（右键/肩键） */
	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* MeleeAction;

	/** 切换武器输入 Action（滚轮/数字键） */
	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* SwitchWeaponAction;

public:

	/** 构造函数 */
	AWeaponCharacter();

	//~ AActor
	virtual void Tick(float DeltaTime) override;

	//~ IAbilitySystemInterface：返回 PlayerState 上的 ASC
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

	//~ 阶段 1 GAS 挂载点
	/** 服务端：Pawn 被 Possess 时初始化 ASC ActorInfo */
	virtual void PossessedBy(AController* NewController) override;

	/** 客户端：PlayerState 复制到达时初始化 ASC ActorInfo */
	virtual void OnRep_PlayerState() override;

	/** 添加输入绑定 */
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

protected:

	/** 缓存的 ASC 指针（实际拥有者是 PlayerState） */
	UPROPERTY()
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	/** 统一的 ASC ActorInfo 初始化：获取 PlayerState 上的 ASC，设置 Owner=PS / Avatar=this */
	void InitAbilitySystem();

	/** Health 归零时的服务端回调：添加 State.Dead Tag、禁用输入/移动、通知 GameMode 重生 */
	virtual void HandleHealthDepleted(AActor* OwningActor);

	/** 检查玩家是否处于死亡状态（ASC 是否持有 State.Dead Tag） */
	bool IsDead() const;

	/** 处理移动输入 */
	void Move(const FInputActionValue& Value);

	/** 处理射击输入：尝试激活 GA_Fire */
	void Fire(const FInputActionValue& Value);

	/** 处理近战输入：尝试激活 GA_Melee */
	void Melee(const FInputActionValue& Value);

	/** 处理切换武器输入：滚轮上/下切换武器 */
	void SwitchWeapon(const FInputActionValue& Value);

	/** 每 Tick 将角色朝向旋转到鼠标在地面的投影位置 */
	void RotateToMouseCursor(float DeltaTime);
};
