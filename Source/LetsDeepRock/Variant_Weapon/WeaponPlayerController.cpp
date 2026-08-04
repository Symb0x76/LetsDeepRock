// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponPlayerController.h"
#include "EnhancedInputSubsystems.h"
#include "InputMappingContext.h"
#include "InputCoreTypes.h"
#include "UI/WeaponDebugTypes.h"
#include "UI/WeaponHUD.h"
#include "UI/WeaponClassSelectWidget.h"
#include "WeaponGameMode.h"
#include "Data/ProfessionSaveGame.h"
#include "Blueprint/UserWidget.h"

AWeaponPlayerController::AWeaponPlayerController()
{
	// 俯视角鼠标瞄准：显示鼠标光标
	bShowMouseCursor = true;
	DefaultMouseCursor = EMouseCursor::Crosshairs;

	// 鼠标输入直接反映到光标位置（不影响 Controller Rotation）
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;

	// 默认职业选择 UI 类（可由蓝图子类替换）
	ClassSelectWidgetClass = UWeaponClassSelectWidget::StaticClass();
}

void AWeaponPlayerController::BeginPlay()
{
	Super::BeginPlay();

	// 关卡内自动显示职业选择 UI：仅本地控制的 PlayerController 需要 UI
	if (IsLocalController())
	{
		ShowClassSelectUI();
	}
}

void AWeaponPlayerController::ShowClassSelectUI()
{
	if (!ClassSelectWidgetClass || ClassSelectWidget)
	{
		return;
	}

	ClassSelectWidget = CreateWidget<UWeaponClassSelectWidget>(this, ClassSelectWidgetClass);
	if (ClassSelectWidget)
	{
		ClassSelectWidget->AddToViewport();
		// 职业选择是模态界面，选择完成前只接收 UI 输入。
		SetInputMode(FInputModeUIOnly().SetWidgetToFocus(ClassSelectWidget->TakeWidget()));
		bShowMouseCursor = true;
	}
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

void AWeaponPlayerController::RequestSelectProfession(FName ProfessionId)
{
	// 本地先完成同一份配置校验与保存；失败时保留界面，允许玩家重新选择。
	if (!UProfessionSaveGame::SelectProfession(this, ProfessionId))
	{
		return;
	}

	// 本地立即隐藏 UI，避免等待服务端往返造成的操作滞后感；
	// 服务端仍会重复校验并执行权威重生。
	if (ClassSelectWidget)
	{
		ClassSelectWidget->RemoveFromParent();
		ClassSelectWidget = nullptr;

		// 恢复纯游戏输入模式（隐藏 UI 交互，WASD/数字键继续可用）
		SetInputMode(FInputModeGameOnly());
		bShowMouseCursor = true; // 俯视角依然需要鼠标瞄准
	}

	ServerSelectProfessionAndRespawn(ProfessionId);
}

void AWeaponPlayerController::ServerSelectProfessionAndRespawn_Implementation(FName ProfessionId)
{
	// 存档为本地单机全局槽位写入，服务端权威执行以匹配单机/监听服务器场景
	if (!UProfessionSaveGame::SelectProfession(this, ProfessionId))
	{
		return;
	}

	if (AWeaponGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AWeaponGameMode>() : nullptr)
	{
		GameMode->RespawnPlayerImmediately(this);
	}
}
