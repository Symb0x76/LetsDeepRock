// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "WeaponMainMenuPlayerController.generated.h"

class UWeaponMainMenuWidget;

/**
 *  主菜单 Player Controller（Variant_Weapon）
 *
 *  只负责创建主菜单 UMG 并把输入切到 UI-only。
 *  没有玩法输入、没有 IMC、不 Possess 任何 Pawn。
 */
UCLASS()
class AWeaponMainMenuPlayerController : public APlayerController
{
	GENERATED_BODY()

public:

	AWeaponMainMenuPlayerController();

protected:

	//~ APlayerController
	virtual void BeginPlay() override;

	/** 主菜单控件类（默认为 UWeaponMainMenuWidget，可由蓝图子类替换） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "MainMenu|UI")
	TSubclassOf<UWeaponMainMenuWidget> MainMenuWidgetClass;

	UPROPERTY()
	TObjectPtr<UWeaponMainMenuWidget> MainMenuWidget;
};
