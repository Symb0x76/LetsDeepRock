// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponPlayerController.h"
#include "EnhancedInputSubsystems.h"
#include "InputMappingContext.h"
#include "InputCoreTypes.h"
#include "UI/WeaponDebugTypes.h"
#include "UI/WeaponHUD.h"

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

#if !UE_BUILD_SHIPPING
	InputComponent->BindKey(EKeys::F8, IE_Pressed, this, &AWeaponPlayerController::ToggleWeaponDebugUI);
#endif
}

void AWeaponPlayerController::ToggleWeaponDebugUI()
{
	SetWeaponDebugUIEnabled(!IsWeaponDebugUIEnabled());
}

void AWeaponPlayerController::ClientReceiveWeaponDamageDebugEvent_Implementation(const FWeaponDamageDebugEvent& Event)
{
#if !UE_BUILD_SHIPPING
	if (IsWeaponDebugUIEnabled())
	{
		if (AWeaponHUD* WeaponHUD = GetHUD<AWeaponHUD>())
		{
			WeaponHUD->AddWeaponDamageDebugEvent(Event);
		}
	}
#endif
}
