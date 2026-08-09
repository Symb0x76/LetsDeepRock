// Copyright Epic Games, Inc. All Rights Reserved.

#include "Variant_Weapon/UI/WeaponMainMenuPlayerController.h"

#include "Blueprint/UserWidget.h"
#include "Variant_Weapon/UI/WeaponMainMenuWidget.h"

AWeaponMainMenuPlayerController::AWeaponMainMenuPlayerController()
{
	// 菜单界面全程需要鼠标
	bShowMouseCursor = true;
	DefaultMouseCursor = EMouseCursor::Default;
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;

	MainMenuWidgetClass = UWeaponMainMenuWidget::StaticClass();
}

void AWeaponMainMenuPlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (IsLocalController())
	{
		ShowMainMenu();
	}
}

void AWeaponMainMenuPlayerController::ShowMainMenu()
{
	if (!MainMenuWidgetClass || MainMenuWidget)
	{
		return;
	}

	MainMenuWidget = CreateWidget<UWeaponMainMenuWidget>(this, MainMenuWidgetClass);
	if (MainMenuWidget)
	{
		MainMenuWidget->AddToViewport();

		// 主菜单是模态界面：只接收 UI 输入，并把焦点交给菜单本体，键盘导航才能生效
		SetInputMode(FInputModeUIOnly().SetWidgetToFocus(MainMenuWidget->TakeWidget()));
		bShowMouseCursor = true;
	}
}
