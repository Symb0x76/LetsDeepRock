// Copyright Epic Games, Inc. All Rights Reserved.

#include "Variant_Weapon/UI/WeaponMainMenuGameMode.h"

#include "GameFramework/WorldSettings.h"
#include "Variant_Weapon/UI/WeaponMainMenuPlayerController.h"

AWeaponMainMenuGameMode::AWeaponMainMenuGameMode()
{
	PlayerControllerClass = AWeaponMainMenuPlayerController::StaticClass();

	// 菜单关卡不需要角色：不生成 Pawn，也不自动 Restart 玩家
	DefaultPawnClass = nullptr;
	bStartPlayersAsSpectators = false;
	HUDClass = nullptr;
}

AActor* AWeaponMainMenuGameMode::FindPlayerStart_Implementation(
	AController* Player,
	const FString& IncomingName)
{
	// InitNewPlayer 无论是否生成 Pawn 都会先调用 FindPlayerStart。
	// 菜单没有 PlayerStart，直接复用引擎默认 fallback，避免输出误导性日志。
	if (UWorld* World = GetWorld())
	{
		return World->GetWorldSettings();
	}
	return this;
}

void AWeaponMainMenuGameMode::RestartPlayer(AController* NewPlayer)
{
	// 主菜单只需要 PlayerController + UMG，不进入 Pawn 生成流程。
}

void AWeaponMainMenuGameMode::InitializeHUDForPlayer_Implementation(APlayerController* NewPlayer)
{
	// GenericPlayerInitialization 默认会 ClientSetHUD(HUDClass)。HUDClass 为空时会触发
	// "SpawnActor failed because no class was specified"；主菜单 UI 已由 PlayerController 创建。
}
