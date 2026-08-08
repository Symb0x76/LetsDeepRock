// Copyright Epic Games, Inc. All Rights Reserved.

#include "Variant_Weapon/UI/WeaponMainMenuGameMode.h"

#include "Variant_Weapon/UI/WeaponMainMenuPlayerController.h"

AWeaponMainMenuGameMode::AWeaponMainMenuGameMode()
{
	PlayerControllerClass = AWeaponMainMenuPlayerController::StaticClass();

	// 菜单关卡不需要角色：不生成 Pawn，也不自动 Restart 玩家
	DefaultPawnClass = nullptr;
	bStartPlayersAsSpectators = false;
	HUDClass = nullptr;
}
