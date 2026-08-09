// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "WeaponMainMenuGameMode.generated.h"

/**
 *  主菜单 Game Mode
 *
 *  使用 AWeaponMainMenuPlayerController，并把 DefaultPawnClass 设为空，
 *  避免在菜单关卡里生成玩法 Pawn。菜单 UI 由 PlayerController 在 BeginPlay 创建。
 */
UCLASS()
class AWeaponMainMenuGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:

	AWeaponMainMenuGameMode();
};
