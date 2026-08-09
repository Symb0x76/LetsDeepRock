// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "WeaponClassSelectWidget.generated.h"

class UBorder;
class UButton;
class UPanelWidget;
class UTextBlock;

/** 携带 ProfessionId 的职业卡片点击事件 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnProfessionCardClicked, FName, ProfessionId);

/**
 *  职业卡片按钮：数量由 DataTable 动态决定，点击时广播职业 ID。
 *
 *  继承自 UButton（非 UUserWidget），NativeOnInitialized 不会被引擎自动调用，
 *  因此不能依赖它来绑定 OnClicked。改为显式的 InitializeCard()，由外部创建后立即调用。
 */
UCLASS()
class UProfessionCardButton : public UButton
{
	GENERATED_BODY()

public:

	UPROPERTY()
	FName ProfessionId;

	UPROPERTY(BlueprintAssignable)
	FOnProfessionCardClicked OnProfessionCardClicked;

	void InitializeCard(FName InProfessionId)
	{
		ProfessionId = InProfessionId;

		if (!bClickBound)
		{
			OnClicked.AddDynamic(this, &UProfessionCardButton::HandleClicked);
			bClickBound = true;
		}
	}

private:

	bool bClickBound = false;

	UFUNCTION()
	void HandleClicked()
	{
		OnProfessionCardClicked.Broadcast(ProfessionId);
	}
};

/**
 *  职业选择终端 UI（Variant_Weapon）—— 单页重做版
 *
 *  设计意图：
 *  - 关卡开始时由 AWeaponPlayerController::BeginPlay 自动创建并 AddToViewport，ZOrder 100。
 *  - 单页布局：上方大型角色模型展示占位（Fill 占主体），画面中间偏下横排职业按钮，
 *    其下是"开始游戏"按钮。
 *  - 玩家点击职业只选中并高亮，不提交；点击"开始游戏"后内部解析该职业默认
 *    Primary/Secondary（默认缺失则取池第一项），再调用
 *    AWeaponPlayerController::RequestSelectProfession(ProfessionId, PrimaryWeaponId, SecondaryWeaponId)
 *    完成存档写入 + 立即重生 + 隐藏 UI。
 *  - 玩家不可见任何武器选择步骤。
 *  - 本类在 NativeOnInitialized 中完全用 C++ UMG 原生构建响应式 UI：
 *    全屏半透明遮罩、SafeZone + ScaleBox 自适应容器，支持 16:9/16:10/21:9 及低分辨率，
 *    文字不裁切，不依赖 WBP 蓝图。
 */
UCLASS()
class UWeaponClassSelectWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	/**
	 *  玩家点击某个职业卡片时调用（由 C++ 按钮 OnClicked 绑定）。
	 *  仅记录职业选择并刷新高亮，不触发重生。
	 */
	UFUNCTION(BlueprintCallable, Category = "Profession")
	void SelectProfession(FName ProfessionId);

protected:

	//~ UUserWidget
	virtual void NativeOnInitialized() override;

private:

	/** 构建单页布局 */
	void BuildLayout(UPanelWidget* RootCanvas);

	/** 构建大型模型展示占位框 */
	UWidget* BuildModelPreviewFrame();

	/** 构建横排职业按钮组 */
	UWidget* BuildProfessionButtonRow();

	/** 构建开始游戏按钮 */
	UWidget* BuildStartGameButton();

	/** 生成一个统一风格的文本块 */
	UTextBlock* MakeText(
		const FString& InText,
		int32 FontSize,
		const FLinearColor& Color,
		bool bBold = false,
		int32 LetterSpacing = 0);

	/** 某个职业卡片被点击 */
	UFUNCTION()
	void HandleProfessionCardClicked(FName ProfessionId);

	/** "开始游戏"点击：解析默认武器并调用 PlayerController::RequestSelectProfession */
	UFUNCTION()
	void HandleStartGameClicked();

	/** 刷新所有职业按钮的选中态高亮 */
	void RefreshProfessionHighlight();

	/** 依据是否已选定职业，更新"开始游戏"按钮启用状态 */
	void RefreshStartGameButtonState();

	/** 当前已选职业 */
	FName SelectedProfessionId;

	/** 职业 Id → 对应卡片 Border（用于刷新选中态高亮） */
	TMap<FName, TObjectPtr<UBorder>> ProfessionCardBorders;

	/** "开始游戏"按钮，随选择状态启用/禁用 */
	UPROPERTY()
	TObjectPtr<UButton> StartGameButton;
};
