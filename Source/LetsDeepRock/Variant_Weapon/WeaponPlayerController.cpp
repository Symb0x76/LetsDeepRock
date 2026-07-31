// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponPlayerController.h"
#include "EnhancedInputSubsystems.h"
#include "InputMappingContext.h"
#include "WeaponGameMode.h"

AWeaponPlayerController::AWeaponPlayerController()
{
	// 俯视角鼠标瞄准：显示鼠标光标
	bShowMouseCursor = true;
	DefaultMouseCursor = EMouseCursor::Crosshairs;

	// 鼠标输入直接反映到光标位置（不影响 Controller Rotation）
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;
}

void AWeaponPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// 注册默认的输入映射上下文
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
		ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
		{
			if (CurrentContext)
			{
				Subsystem->AddMappingContext(CurrentContext, 0);
			}
		}
	}
}

void AWeaponPlayerController::PawnLeavingGame()
{
	if (AWeaponGameMode* WeaponGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AWeaponGameMode>() : nullptr;
		WeaponGameMode && WeaponGameMode->PreserveLeavingPlayerAttribution(this))
	{
		return;
	}
	Super::PawnLeavingGame();
}
