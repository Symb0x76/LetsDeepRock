// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "WeaponClassSelectWidget.generated.h"

class UWidgetSwitcher;
class UVerticalBox;
class UHorizontalBox;
class UButton;
class UBorder;
class UTextBlock;

/** 携带 SlotType/WeaponId 的武器卡片点击事件（供 UWeaponCardButton 广播） */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnWeaponCardClicked, FName, SlotType, FName, WeaponId);

/**
 *  武器选择页专用的卡片按钮：数量由 DataTable 动态决定，无法为每个实例声明独立 UFUNCTION，
 *  因此让按钮自身持有 SlotType/WeaponId，点击时转译为携带参数的 FOnWeaponCardClicked 广播，
 *  宿主 Widget 只需订阅一次即可统一处理任意数量的卡片。
 *
 *  注意：本类继承自 UButton（非 UUserWidget），NativeOnInitialized 在其作为普通子控件
 *  被 WidgetTree->ConstructWidget<>() 创建时不会被引擎自动调用，因此不能依赖它来绑定
 *  OnClicked。改为显式的 InitializeCard()，由外部创建后立即调用一次完成配置与绑定。
 */
UCLASS()
class UWeaponCardButton : public UButton
{
	GENERATED_BODY()

public:
	/** 该卡片对应的槽位（"Primary"/"Secondary"）与武器 Id，通过 InitializeCard 设置 */
	UPROPERTY()
	FName SlotType;

	UPROPERTY()
	FName WeaponId;

	/** 点击时广播 SlotType/WeaponId，供宿主 Widget 统一订阅 */
	UPROPERTY(BlueprintAssignable)
	FOnWeaponCardClicked OnWeaponCardClicked;

	/**
	 *  显式初始化：设置槽位与武器 Id，并确保 OnClicked 只绑定一次。
	 *  必须在 WidgetTree->ConstructWidget<UWeaponCardButton>() 创建后立即调用。
	 */
	void InitializeCard(FName InSlotType, FName InWeaponId)
	{
		SlotType = InSlotType;
		WeaponId = InWeaponId;

		if (!bClickBound)
		{
			OnClicked.AddDynamic(this, &UWeaponCardButton::HandleClicked);
			bClickBound = true;
		}
	}

private:
	/** 防止重复调用 InitializeCard 时重复绑定 OnClicked */
	bool bClickBound = false;

	UFUNCTION()
	void HandleClicked()
	{
		OnWeaponCardClicked.Broadcast(SlotType, WeaponId);
	}
};

/**
 *  职业 → 武器选择终端 UI（Variant_Weapon）
 *
 *  设计意图：
 *  - 关卡开始时由 AWeaponPlayerController::BeginPlay 自动创建并 AddToViewport，无需玩家手动呼出。
 *  - 第一页：Ranger / Demolitionist / Engineer 三职业卡片，点击仅记录职业并切到第二页，不立即重生。
 *  - 第二页：从 UGameDataSubsystem::GetProfessionWeaponIds 动态展示该职业合法的 Primary / Secondary
 *    武器池，各槽单选，默认预选 GetDefaultProfessionWeaponId；"返回"清空选择回第一页；
 *    主副武器均选定后启用"开始游戏"，调用
 *    AWeaponPlayerController::RequestSelectProfession(ProfessionId, PrimaryWeaponId, SecondaryWeaponId)
 *    完成存档写入 + 立即重生 + 隐藏 UI。
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
	 *  仅记录职业选择并切换到武器选择页，不触发重生。
	 */
	UFUNCTION(BlueprintCallable, Category = "Profession")
	void SelectProfession(FName ProfessionId);

protected:

	//~ UUserWidget
	virtual void NativeOnInitialized() override;

private:

	// ---- 第一页：职业选择 ----

	// 按钮点击辅助方法（分别对应三个职业）
	UFUNCTION()
	void OnRangerClicked();

	UFUNCTION()
	void OnDemolitionistClicked();

	UFUNCTION()
	void OnEngineerClicked();

	// ---- 页面切换 ----

	/** 承载两页内容的切换容器 */
	UPROPERTY()
	TObjectPtr<UWidgetSwitcher> PageSwitcher;

	/** 武器选择页根容器，每次进入时清空重建（内容依赖已选职业） */
	UPROPERTY()
	TObjectPtr<UVerticalBox> WeaponPageRoot;

	/** "开始游戏"按钮，随选择状态启用/禁用 */
	UPROPERTY()
	TObjectPtr<UButton> StartGameButton;

	static constexpr int32 ProfessionPageIndex = 0;
	static constexpr int32 WeaponPageIndex = 1;

	// ---- 第二页：武器选择状态 ----

	/** 当前已选职业（第一页点击后写入） */
	FName SelectedProfessionId;

	/** 当前已选主武器 / 副武器 Id，未选定为 NAME_None */
	FName SelectedPrimaryWeaponId;
	FName SelectedSecondaryWeaponId;

	/** Primary/Secondary 槽位内，武器 Id → 对应卡片按钮 Border（用于刷新选中态高亮） */
	TMap<FName, TObjectPtr<UBorder>> PrimaryWeaponCardBorders;
	TMap<FName, TObjectPtr<UBorder>> SecondaryWeaponCardBorders;

	/** 进入武器选择页：依据 SelectedProfessionId 动态生成两组武器卡片 */
	void BuildWeaponSelectPage();

	/** 某个槽位的武器卡片被点击（订阅自所有 UWeaponCardButton::OnWeaponCardClicked） */
	UFUNCTION()
	void HandleWeaponCardClicked(FName SlotType, FName WeaponId);

	/** 返回职业选择页并清空武器选择状态（由返回按钮 OnClicked.AddDynamic 绑定，须为 UFUNCTION） */
	UFUNCTION()
	void HandleBackToProfessionClicked();

	/** "开始游戏"点击：调用 PlayerController::RequestSelectProfession 完成存档写入 + 重生
	 *  （由开始游戏按钮 OnClicked.AddDynamic 绑定，须为 UFUNCTION） */
	UFUNCTION()
	void HandleStartGameClicked();

	/** 刷新指定槽位内所有卡片的选中态高亮 */
	void RefreshWeaponSlotHighlight(const TMap<FName, TObjectPtr<UBorder>>& CardBorders, FName SelectedWeaponId);

	/** 依据两槽是否均已选定有效武器，更新"开始游戏"按钮启用状态 */
	void RefreshStartGameButtonState();
};
