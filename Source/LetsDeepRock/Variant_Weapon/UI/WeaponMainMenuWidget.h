// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "WeaponMainMenuWidget.generated.h"

class UBorder;
class UButton;
class UPanelWidget;
class UTextBlock;
class UVerticalBox;

/**
 *  主菜单 UI（Variant_Weapon）
 *
 *  由 AWeaponMainMenuPlayerController::BeginPlay 创建，纯 C++ UMG 构建，不依赖 WBP。
 *
 *  两个入口：
 *  - 单人游戏：直接 OpenLevel 到玩法关卡，不建 Session。
 *  - 多人游戏：UWeaponSessionSubsystem::HostLanGame(4, 房间名, 目标地图) 以 listen 模式进入同一关卡。
 *
 *  视觉沿用房间 UI 的工业科幻语言：深色底 + 橙黄强调 + 拉开字距的英文副标签，
 *  但主菜单刻意保持克制——大量留白，只有两个按钮。
 */
UCLASS()
class UWeaponMainMenuWidget : public UUserWidget
{
	GENERATED_BODY()

protected:

	//~ UUserWidget
	virtual void NativeOnInitialized() override;

private:

	/** 构建整个菜单布局 */
	void BuildLayout(UPanelWidget* RootCanvas);

	/**
	 *  生成一个主菜单大按钮（中文主标题 + 英文副标签 + 左侧橙黄竖条）。
	 *  Parent 必须是 UVerticalBox：内部直接使用 AddChildToVerticalBox 取得
	 *  UVerticalBoxSlot 以设置 Padding 与对齐。
	 */
	UButton* BuildMenuButton(
		UVerticalBox* Parent,
		const FString& Title,
		const FString& Subtitle,
		bool bIsPrimary);

	UTextBlock* MakeText(
		const FString& InText,
		int32 FontSize,
		const FLinearColor& Color,
		bool bBold = false,
		int32 LetterSpacing = 0);

	UFUNCTION()
	void HandleSinglePlayerClicked();

	UFUNCTION()
	void HandleMultiplayerClicked();

	/** 状态提示：多人开房失败时给玩家反馈，避免点了没反应 */
	UPROPERTY()
	TObjectPtr<UTextBlock> StatusText = nullptr;

	/** 防止连点造成重复 OpenLevel / HostLanGame */
	bool bTransitionStarted = false;
};
