// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponMainMenuGameMode.h"

#include "WeaponMainMenuPlayerController.h"

AWeaponMainMenuGameMode::AWeaponMainMenuGameMode()
{
	PlayerControllerClass = AWeaponMainMenuPlayerController::StaticClass();

	// 主菜单不需要任何 Pawn；置空避免引擎生成默认 DefaultPawn。
	DefaultPawnClass = nullptr;
}
