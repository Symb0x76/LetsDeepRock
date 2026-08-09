// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponMainMenuPlayerController.h"

#include "Blueprint/UserWidget.h"
#include "UI/WeaponMainMenuWidget.h"

AWeaponMainMenuPlayerController::AWeaponMainMenuPlayerController()
{
	bShowMouseCursor = true;
	DefaultMouseCursor = EMouseCursor::Default;
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;

	MainMenuWidgetClass = UWeaponMainMenuWidget::StaticClass();
}

void AWeaponMainMenuPlayerController::BeginPlay()
{
	Super::BeginPlay();

	if (!IsLocalController() || !MainMenuWidgetClass || MainMenuWidget)
	{
		return;
	}

	MainMenuWidget = CreateWidget<UWeaponMainMenuWidget>(this, MainMenuWidgetClass);
	if (MainMenuWidget)
	{
		MainMenuWidget->AddToViewport();

		// 主菜单是纯 UI 场景，输入全部交给 UMG。
		SetInputMode(FInputModeUIOnly().SetWidgetToFocus(MainMenuWidget->TakeWidget()));
		bShowMouseCursor = true;
	}
}
