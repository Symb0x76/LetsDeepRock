// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "UI/WeaponDebugTypes.h"
#include "WeaponPlayerController.generated.h"

class UInputMappingContext;
class UWeaponClassSelectWidget;

/**
 *  俯视角武器系统 Player Controller（Variant_Weapon 骨架）
 *
 *  阶段 0：仅负责注册 Enhanced Input 的 IMC。
 *  后续阶段将扩展：武器输入 InputTag 分发、死亡重生等。
 *
 *  3C 优化：启用鼠标光标显示，角色始终面向鼠标位置。
 *
 *  职业选择：关卡开始时（BeginPlay）自动创建并显示 UWeaponClassSelectWidget，
 *  玩家点击职业后调用 ServerSelectProfessionAndRespawn 写入存档并立即重生。
 */
UCLASS(abstract, Config = "Game")
class AWeaponPlayerController : public APlayerController
{
	GENERATED_BODY()

public:

	AWeaponPlayerController();

	/** 接收服务端权威伤害结果，仅用于本地 Debug HUD。 */
	UFUNCTION(Client, Unreliable)
	void ClientReceiveWeaponDamageDebugEvent(const FWeaponDamageDebugEvent& Event);

	/**
	 *  客户端调用：玩家在职业选择 UI 上点击某个职业。
	 *  内部转发到服务端 RPC 完成存档写入 + 立即重生；
	 *  本地额外立即隐藏 UI，避免等待网络往返造成的操作滞后感。
	 */
	UFUNCTION(BlueprintCallable, Category = "Profession")
	void RequestSelectProfession(FName ProfessionId, FName PrimaryWeaponId, FName SecondaryWeaponId);

protected:

	/** 默认输入映射上下文 */
	UPROPERTY(EditAnywhere, Category = "Input|Input Mappings")
	TArray<UInputMappingContext*> DefaultMappingContexts;

	/** 职业选择界面控件类（默认为 UWeaponClassSelectWidget，可由蓝图子类替换） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Profession|UI")
	TSubclassOf<UWeaponClassSelectWidget> ClassSelectWidgetClass;

	/** 当前显示中的职业选择控件实例（仅本地客户端持有） */
	UPROPERTY()
	TObjectPtr<UWeaponClassSelectWidget> ClassSelectWidget;

protected:

	//~ APlayerController
	virtual void BeginPlay() override;

	/** 初始化输入绑定 */
	virtual void SetupInputComponent() override;

	/** F8 切换 Weapon.DebugUI。 */
	void ToggleWeaponDebugUI();

	/** 关卡内自动创建并显示职业选择 UI（仅本地控制器执行） */
	void ShowClassSelectUI();

	/** 恢复俯视角玩法输入：保留可见光标，同时让未被 UI 处理的输入进入游戏。 */
	void ApplyGameplayInputMode();

	/** 服务端：写入职业存档并立即重生当前 Pawn */
	UFUNCTION(Server, Reliable)
	void ServerSelectProfessionAndRespawn(FName ProfessionId, FName PrimaryWeaponId, FName SecondaryWeaponId);
};
