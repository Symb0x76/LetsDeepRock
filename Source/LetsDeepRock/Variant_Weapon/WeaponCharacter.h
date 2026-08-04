// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "AbilitySystemInterface.h"
#include "GridMap/GridMapTypes.h"
#include "GameplayAbilitySpec.h"
#include "WeaponCharacter.generated.h"

class USpringArmComponent;
class UCameraComponent;
struct FInputActionValue;
class UInputAction;
class UInputMappingContext;
class UAbilitySystemComponent;
class UWeaponEquipmentComponent;
class UGameplayAbility;
class UGrenadeTrajectoryPreviewComponent;
struct FCharacterAttributeRow;
class UWeaponGrenadeDataAsset;
class UVisionSourceComponent;
class ADrone;
struct FGrenadeBallisticsOutput;
struct FProfessionConfigRow;

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

	/** 角色锥形视野来源。 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UVisionSourceComponent> VisionSource;

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

	/** 手雷投掷输入 Action */
	UPROPERTY(EditAnywhere, Category = "Input")
	UInputAction* ThrowGrenadeAction;

	/** 手雷投掷 Ability 类（由蓝图配置，独立于武器装备系统） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grenade")
	TSubclassOf<UGameplayAbility> GrenadeAbilityClass;

	/** 手雷数据资产（由蓝图配置或从 Ability CDO 读取，供水前预览组件使用） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grenade")
	TObjectPtr<UWeaponGrenadeDataAsset> GrenadeDataAsset;

	/** 是否启用手雷弹道预览（临时调试开关） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Grenade")
	bool bEnableGrenadePreview = false;

	/** 职业工具无人机类；未配置时不部署 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Profession Tool")
	TSubclassOf<ADrone> ProfessionToolDroneClass;

public:

	/** 构造函数 */
	AWeaponCharacter();

	//~ AActor
	virtual void Tick(float DeltaTime) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	//~ IAbilitySystemInterface：返回 PlayerState 上的 ASC
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

	//~ 阶段 1 GAS 挂载点
	/** 服务端：Pawn 被 Possess 时初始化 ASC ActorInfo */
	virtual void PossessedBy(AController* NewController) override;

	/** 客户端：PlayerState 复制到达时初始化 ASC ActorInfo */
	virtual void OnRep_PlayerState() override;

	/** 添加输入绑定 */
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;

	/** 返回服务器确认并复制的水平瞄准方向；射击和近战统一读取此值。 */
	UFUNCTION(BlueprintPure, Category = "Combat|Aim")
	FVector GetAimDirection() const { return FVector(AimDirection); }

	/** 服务器写入瞄准方向，并同步角色的水平 Rotation。 */
	void SetAimDirectionAuthoritative(const FVector& NewAimDirection);

	/** 快照兼容接口：返回职业配置派生出的属性行，不维护独立复制配置源。 */
	FName GetCharacterConfigRowName() const { return ActiveCharacterAttributeRow; }
	bool SetCharacterConfigRowNameAuthoritative(FName NewRowName);

	/** PlayerState 职业状态复制到达后刷新派生属性表现；客户端不会建立库存。 */
	void RefreshProfessionLoadout();

	uint8 GetCurrentEquipmentSlot() const { return CurrentEquipmentSlot; }
	bool RestoreEquipmentSlot(uint8 SlotIndex);

	/** 本地玩家提交建造意图；服务器负责距离、格子状态和类型验证。 */
	UFUNCTION(BlueprintCallable, Category = "GridMap")
	void RequestBuildTileAtWorldLocation(const FVector& WorldLocation, EGridTileType BuildType);
	
	/** GA 调用：消费一次性 pending 手雷目标，返回 true 表示有有效目标 */
	bool ConsumePendingGrenadeTarget(FVector& OutTarget, bool& bOutValid) const;

protected:

	/** 缓存的 ASC 指针（实际拥有者是 PlayerState） */
	UPROPERTY()
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	/** 已授予的手雷 Ability Handle（EndPlay Authority 端清除，重生时重新 Grant） */
	FGameplayAbilitySpecHandle GrenadeAbilityHandle;

	/** 统一的 ASC ActorInfo 初始化：获取 PlayerState 上的 ASC，设置 Owner=PS / Avatar=this */
	void InitAbilitySystem();

	/** 按职业配置和存档初始化主副武器 */
	void InitProfessionWeapons(const FProfessionConfigRow* ProfessionRow, FName PrimaryWeaponId, FName SecondaryWeaponId);

	/** Health 归零时的服务端回调：添加 State.Dead Tag、禁用输入/移动、通知 GameMode 重生 */
	virtual void HandleHealthDepleted(AActor* OwningActor);

	/** 属性是否已从表初始化（避免重复初始化） */
	bool bAttributesInitialized = false;

	/** 当前职业配置派生出的属性行，仅供快照一致性校验。 */
	FName ActiveCharacterAttributeRow = NAME_None;

	/** 检查玩家是否处于死亡状态（ASC 是否持有 State.Dead Tag） */
	bool IsDead() const;

	/** 处理移动输入 */
	void Move(const FInputActionValue& Value);

	/** 左键按不同装备槽分发输入 */
	void FireStarted(const FInputActionValue& Value);
	void FireTriggered(const FInputActionValue& Value);
	void FireCompleted(const FInputActionValue& Value);
	void FireCanceled(const FInputActionValue& Value);

	/** 原始鼠标左键诊断监听，仅确认物理按键事件是否到达。 */
	void RawFirePressed();
	void RawFireReleased();

#if !UE_BUILD_SHIPPING
	/** 输出当前射击输入、装备和弹药状态。 */
	void LogFireTraceState(const TCHAR* EventName) const;
#endif

	/** 当前武器是否配置为按住射击。 */
	bool IsCurrentWeaponHoldTrigger() const;

	/** 主副武器尝试射击 */
	void FireWeapon();

	/** 处理近战输入：尝试激活 GA_Melee */
	void Melee(const FInputActionValue& Value);

	/** 处理切换武器输入：滚轮上/下切换武器 */
	void SwitchWeapon(const FInputActionValue& Value);

	/** R 键手动换弹。 */
	void ReloadWeapon();

	UFUNCTION(Server, Reliable)
	void ServerReloadWeapon();

	/** 选择四个数字装备槽 */
	void SelectPrimary();
	void SelectSecondary();
	void SelectSkill();
	void SelectTool();
	void SelectEquipmentSlot(uint8 SlotIndex);

	UFUNCTION(Server, Reliable)
	void ServerSelectEquipmentSlot(uint8 SlotIndex);

	UFUNCTION()
	void OnRep_CurrentEquipmentSlot();

	/** 在服务端部署职业工具无人机 */
	void DeployProfessionTool();

	UFUNCTION(Server, Reliable)
	void ServerDeployProfessionTool();

	/** 处理手雷投掷输入：Started（预览）/ Triggered（更新）/ Completed（投掷）/ Canceled（取消） */
	void ThrowGrenadeStarted(const FInputActionValue& Value);
	void ThrowGrenadeTriggered(const FInputActionValue& Value);
	void ThrowGrenadeCompleted(const FInputActionValue& Value);
	void ThrowGrenadeCanceled(const FInputActionValue& Value);

	// === 手雷预览（仅本地控制 Pawn 执行） ===

	/** 获取鼠标地面位置（通过 Visibility 通道射线检测），返回是否命中地面 */
	bool GetMouseGroundPosition(FVector& OutGroundPoint) const;

	/** 执行弹道解算并显示预览 */
	void ShowGrenadePreview();

	/** 更新预览（按住期间鼠标移动时调用） */
	void UpdateGrenadePreview();

	/** 隐藏预览 */
	void HideGrenadePreview();

	// === 手雷 Server RPC ===

	/** 客户端→服务器：发送瞄准目标，服务端校验后激活 GA */
	UFUNCTION(Server, Reliable)
	void ServerThrowGrenade(FVector_NetQuantize TargetPoint, bool bHasValidTarget);

	/** 每 Tick 将角色朝向旋转到鼠标在地面的投影位置 */
	void RotateToMouseCursor(float DeltaTime);

	/** 客户端提交本地瞄准意图，服务器校验后写入权威方向。 */
	UFUNCTION(Server, Unreliable)
	void ServerSetAimDirection(FVector_NetQuantizeNormal NewAimDirection);

	UFUNCTION()
	void OnRep_AimDirection();

	/** 当前服务器确认的水平瞄准方向。 */
	UPROPERTY(ReplicatedUsing = OnRep_AimDirection, BlueprintReadOnly, Category = "Combat|Aim")
	FVector_NetQuantizeNormal AimDirection = FVector(1.0f, 0.0f, 0.0f);

	UFUNCTION(Server, Reliable)
	void ServerRequestBuildTile(FVector_NetQuantize WorldLocation, EGridTileType BuildType);

	// === 手雷预览 ===

	/** 弹道预览组件（仅本地控制 Pawn 创建，非复制） */
	UPROPERTY()
	TObjectPtr<UGrenadeTrajectoryPreviewComponent> GrenadePreviewComponent;

	/** 是否正在预览（按住 G 键期间） */
	bool bIsPreviewingGrenade = false;

	/** 最近一次预览解算使用的目标点，Completed 时沿同一路径投掷 */
	mutable FVector PreviewGrenadeTarget = FVector::ZeroVector;

	/** 最近一次预览目标是否有效 */
	mutable bool bHasPreviewGrenadeTarget = false;

	// === 手雷 Pending Target（服务器消费） ===

	/** 一次性待消费手雷目标点（服务器 RPC 设置，GA 消费后清空） */
	mutable FVector PendingGrenadeTarget = FVector::ZeroVector;

	/** PendingGrenadeTarget 是否有效 */
	mutable bool bHasPendingGrenadeTarget = false;

	/** 当前装备槽：由服务端权威选择并复制。 */
	UPROPERTY(ReplicatedUsing = OnRep_CurrentEquipmentSlot)
	uint8 CurrentEquipmentSlot = 0;

	/** 本次左键按下时使用的槽；255 表示当前没有持续中的左键输入 */
	uint8 ActiveFireInputSlot = 255;

	/** 每次按压只记录一次 Enhanced Input Triggered，避免按住时逐帧刷屏。 */
	bool bFireTriggeredLoggedThisPress = false;

	/** 当前职业是否配置了技能道具和职业工具 */
	bool bHasSkillItem = false;
	bool bHasProfessionTool = false;

	/** 当前已部署的独立职业工具无人机 */
	UPROPERTY()
	TObjectPtr<ADrone> ActiveDrone;
};
