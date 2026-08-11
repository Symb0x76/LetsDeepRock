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

	/** 菜单没有 PlayerStart；返回 WorldSettings 作为无日志的控制器初始位置锚点。 */
	virtual AActor* FindPlayerStart_Implementation(AController* Player, const FString& IncomingName) override;

	/** 菜单关卡没有 Pawn；吞掉 GameModeBase 的默认 RestartPlayer 流程。 */
	virtual void RestartPlayer(AController* NewPlayer) override;

	/** 菜单使用纯 UMG，不创建 AHUD。 */
	virtual void InitializeHUDForPlayer_Implementation(APlayerController* NewPlayer) override;
};
