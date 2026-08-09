// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "WeaponMainMenuGameMode.generated.h"

/**
 *  主菜单 Game Mode（Variant_Weapon）
 *
 *  MainMenu.umap 的 World Settings 已引用 /Script/LetsDeepRock.WeaponMainMenuGameMode，
 *  本类的存在使该引用得以解析。
 *
 *  主菜单没有玩法：不生成 Pawn，只需要一个能挂 UMG 并接收鼠标输入的 PlayerController。
 *  DefaultPawnClass 显式设为空，避免引擎在空关卡里生成默认 DefaultPawn。
 */
UCLASS()
class AWeaponMainMenuGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:

	AWeaponMainMenuGameMode();
};
