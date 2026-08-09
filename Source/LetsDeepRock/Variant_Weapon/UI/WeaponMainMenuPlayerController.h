// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "WeaponMainMenuPlayerController.generated.h"

class UWeaponMainMenuWidget;

/**
 *  主菜单专用 Player Controller
 *
 *  BeginPlay 时创建并显示 UWeaponMainMenuWidget，输入模式设为 UIOnly 并显示鼠标。
 *  菜单关卡不需要玩法输入，也不需要 Pawn，所以这里不注册任何 IMC。
 */
UCLASS()
class AWeaponMainMenuPlayerController : public APlayerController
{
	GENERATED_BODY()

public:

	AWeaponMainMenuPlayerController();

protected:

	//~ APlayerController
	virtual void BeginPlay() override;

	/** 主菜单控件类（默认 UWeaponMainMenuWidget，可由蓝图子类替换） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Main Menu|UI")
	TSubclassOf<UWeaponMainMenuWidget> MainMenuWidgetClass;

	/** 当前显示中的主菜单实例 */
	UPROPERTY()
	TObjectPtr<UWeaponMainMenuWidget> MainMenuWidget;

private:

	/** 创建主菜单并切换到 UIOnly 输入模式 */
	void ShowMainMenu();
};
