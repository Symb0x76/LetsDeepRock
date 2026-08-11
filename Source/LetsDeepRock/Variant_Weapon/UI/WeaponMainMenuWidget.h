// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Components/Button.h"
#include "Engine/TimerHandle.h"
#include "Online/WeaponCentralConnectivityTypes.h"
#include "Online/WeaponSessionSubsystem.h"
#include "WeaponMainMenuWidget.generated.h"

class UCanvasPanel;
class UEditableTextBox;
class UPanelWidget;
class UTextBlock;
class UVerticalBox;
class UWidgetSwitcher;
class UWeaponCentralConnectivitySubsystem;
class UWeaponSessionSubsystem;

/** 房间行 Join 按钮点击事件，携带 FindLanGames 返回的结果索引。 */
DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnWeaponRoomJoinClicked, int32, ResultIndex);

/**
 *  房间列表里的加入按钮：房间数量由搜索结果决定，无法为每行声明独立 UFUNCTION，
 *  因此让按钮自身记住 ResultIndex，点击时广播出去，宿主 Widget 只订阅一次。
 *
 *  注意：本类继承自 UButton（不是 UUserWidget），作为普通子控件创建时引擎不会调用
 *  NativeOnInitialized，所以必须在创建后显式调用 InitializeRow 完成绑定。
 */
UCLASS()
class UWeaponMainMenuRoomJoinButton : public UButton
{
	GENERATED_BODY()

public:
	/** 对应最近一次 LAN 搜索结果的索引 */
	UPROPERTY()
	int32 ResultIndex = INDEX_NONE;

	/** 点击时广播 ResultIndex */
	UPROPERTY()
	FOnWeaponRoomJoinClicked OnRoomJoinClicked;

	/** 显式初始化：记录索引并保证 OnClicked 只绑定一次 */
	void InitializeRow(int32 InResultIndex)
	{
		ResultIndex = InResultIndex;

		if (!bClickBound)
		{
			OnClicked.AddDynamic(this, &UWeaponMainMenuRoomJoinButton::HandleSelfClicked);
			bClickBound = true;
		}
	}

private:
	bool bClickBound = false;

	UFUNCTION()
	void HandleSelfClicked()
	{
		OnRoomJoinClicked.Broadcast(ResultIndex);
	}
};

/** 创建房间的推进状态：先连中心，失败可回退 LocalP2P。 */
UENUM()
enum class EWeaponRoomCreateState : uint8
{
	Idle,
	ConnectingCentral,
	JoiningCentral,
	FallbackPrompt,
};

/**
 *  中心连接失败后的 LocalP2P 回退确认弹窗。
 *
 *  纯 C++ UMG 模态层，创建后 AddToViewport 置于主菜单之上，点击"接受"或"拒绝"后销毁自身。
 */
UCLASS()
class UWeaponLobbyFallbackDialog : public UUserWidget
{
	GENERATED_BODY()

public:
	/** 显示"是否以 LocalP2P 模式继续？"；接受继续创建房间，拒绝留在主菜单。 */
	void ShowFallbackDialog(UWidget* InOwningMenu, UWidget* InCreateButton);

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_OneParam(FOnFallbackChosen, bool, bAccepted);

	/** 用户选择结果广播给宿主菜单。 */
	UPROPERTY()
	FOnFallbackChosen OnFallbackChosen;

protected:
	virtual void NativeOnInitialized() override;

private:
	UFUNCTION()
	void HandleAcceptClicked();

	UFUNCTION()
	void HandleDeclineClicked();

	UPROPERTY()
	TObjectPtr<UWidget> OwningMenu = nullptr;

	UPROPERTY()
	TObjectPtr<UWidget> CreateButton = nullptr;
};

/**
 *  纯 C++ UMG 主菜单（Variant_Weapon）
 *
 *  视觉：暗色科幻矿业终端风格，琥珀黄强调色。左侧是身份栏 + LDR 大标题 + 纵向导航，
 *  右半侧是一块深色面板，承载各子页面。整体在 1920x1080 的设计画布上排版，
 *  外层用 SafeZone + ScaleBox 等比缩放，保证 16:9 构图在不同分辨率下不变形、不裁切。
 *
 *  交互：
 *  - 单人模式：直接 OpenLevel 到玩法关卡。
 *  - 多人模式：右侧切到组队面板，复用 UWeaponSessionSubsystem 做创建房间 / 刷新 LAN / 加入。
 *    创建房间由 Session 子系统前往玩法关卡，并在 listen NetDriver 就绪后广播 LAN Session。
 *  - 选项设置 / 百科：切到占位页，只有标题、暂未开放提示和右上角返回。
 *  - 退出游戏：UKismetSystemLibrary::QuitGame。
 */
UCLASS()
class UWeaponMainMenuWidget : public UUserWidget
{
	GENERATED_BODY()

protected:

	//~ UUserWidget
	virtual void NativeOnInitialized() override;
	virtual void NativeDestruct() override;

private:

	// ---- 布局构建 ----

	/** 在 1920x1080 的设计画布上摆放全部内容 */
	void BuildDesignCanvas(UCanvasPanel* DesignCanvas);

	/** 左上：头像占位 + 玩家名 + 管理存档按钮 */
	void BuildIdentityBar(UCanvasPanel* DesignCanvas);

	/** 左侧中上：LDR 大标题 */
	void BuildBrandBlock(UCanvasPanel* DesignCanvas);

	/** 左下：纵向主导航按钮 */
	void BuildNavColumn(UCanvasPanel* DesignCanvas);

	/** 右半侧深色面板：标题栏 + 返回按钮 + 页面切换器 */
	void BuildRightPanel(UCanvasPanel* DesignCanvas);

	/** 右侧页面 0：未设计占位 */
	void BuildIdlePage();

	/** 右侧页面 1：组队 */
	void BuildTeamPage();

	/** 右侧页面 2/3：暂未开放占位 */
	UVerticalBox* BuildComingSoonPage(const FString& Hint);

	// ---- 控件工厂 ----

	/** 左侧导航按钮：定高、左对齐、带琥珀色指示条 */
	UButton* MakeNavButton(const FString& Label, const FLinearColor& MarkerColor);

	/** 面板内的操作按钮：紧凑尺寸，可选实心强调样式 */
	UButton* MakeActionButton(const FString& Label, bool bFilled);

	/** 统一的文本构造入口 */
	UTextBlock* MakeText(const FString& Text, const FName Typeface, float FontSize, const FLinearColor& Color, int32 LetterSpacing = 0);

	// ---- 页面切换 ----

	/** 切换右侧面板页面并更新标题、返回按钮可见性 */
	void ShowPanelPage(int32 PageIndex, const FString& Title);

	// ---- 导航按钮回调 ----

	UFUNCTION()
	void HandleSinglePlayerClicked();

	UFUNCTION()
	void HandleMultiplayerClicked();

	UFUNCTION()
	void HandleOptionsClicked();

	UFUNCTION()
	void HandleCodexClicked();

	UFUNCTION()
	void HandleQuitClicked();

	UFUNCTION()
	void HandleBackClicked();

	// ---- 组队面板回调 ----

	UFUNCTION()
	void HandleCreateRoomClicked();

	UFUNCTION()
	void HandleRefreshRoomsClicked();

	/** 使用玩家输入的 Central JoinCode 加入互联网房间。 */
	UFUNCTION()
	void HandleCentralJoinClicked();

	UFUNCTION()
	void HandleRoomJoinClicked(int32 ResultIndex);

	/** 订阅 UWeaponSessionSubsystem::OnHostComplete */
	UFUNCTION()
	void HandleHostSessionComplete(bool bWasSuccessful, FString Message);

	/** 订阅 UWeaponSessionSubsystem::OnFindComplete */
	UFUNCTION()
	void HandleFindSessionsComplete(bool bWasSuccessful, const TArray<FWeaponLanSessionResult>& Results);

	/** 订阅 UWeaponSessionSubsystem::OnJoinComplete */
	UFUNCTION()
	void HandleJoinSessionComplete(bool bWasSuccessful, FString Message);

	/** 订阅 UWeaponCentralConnectivitySubsystem::OnConnectivityChanged */
	UFUNCTION()
	void HandleCentralConnectivityChanged(EWeaponCentralConnectivityState NewState, FString Message);

	/** LocalP2P 回退弹窗的选择结果 */
	UFUNCTION()
	void HandleFallbackChosen(bool bAccepted);

	// ---- 创建房间流程 ----

	/** 进入"连接中心"状态：发起中心认证，并启动就绪等待计时。 */
	void BeginConnectingCentral();

	/** 中心控制认证 + MatchId 就绪则进入游戏；Lease / Checkpoint 在玩法世界内继续收敛。 */
	void CheckCentralReadyAndMaybeStart();

	/** 就绪或用户接受回退后，真正发起 HostLanGameOnMap。 */
	void StartHostingGame();

	/** 超时 / 中心失败 / 用户拒绝时进入回退询问弹窗。 */
	void PromptLocalP2PFallback(const FString& Reason);

	/** 中心等待计时到点（尚未就绪）→ 弹窗询问 LocalP2P。 */
	void HandleCentralWaitTimeout();

	// ---- 辅助 ----

	UWeaponSessionSubsystem* GetSessionSubsystem() const;

	/** 依据搜索结果重建房间行；结果为空时显示空态提示 */
	void RebuildRoomList(const TArray<FWeaponLanSessionResult>& Results);

	void SetRoomStatus(const FString& Status);

	// ---- 控件引用 ----

	/** 右侧面板的页面切换器 */
	UPROPERTY()
	TObjectPtr<UWidgetSwitcher> PanelSwitcher;

	/** 右侧面板标题（随页面变化） */
	UPROPERTY()
	TObjectPtr<UTextBlock> PanelTitleText;

	/** 右上角返回按钮（页面 0 时隐藏） */
	UPROPERTY()
	TObjectPtr<UButton> PanelBackButton;

	/** 组队面板：房间行容器 */
	UPROPERTY()
	TObjectPtr<UVerticalBox> RoomListBox;

	/** 组队面板：状态提示文本 */
	UPROPERTY()
	TObjectPtr<UTextBlock> RoomStatusText;

	/** 回退确认弹窗实例（仅创建流程中非空） */
	UPROPERTY()
	TObjectPtr<UWeaponLobbyFallbackDialog> FallbackDialog;

	/** 组队面板的"创建房间"按钮（流程期间禁用防重复点击） */
	UPROPERTY()
	TObjectPtr<UButton> CreateRoomButton;

	/** 组队面板：玩家输入的 Central JoinCode。 */
	UPROPERTY()
	TObjectPtr<UEditableTextBox> CentralJoinCodeInput;

	/** 组队面板：按 JoinCode 加入中心房间。 */
	UPROPERTY()
	TObjectPtr<UButton> CentralJoinButton;

	/** 当前创建房间流程状态 */
	EWeaponRoomCreateState RoomCreateState = EWeaponRoomCreateState::Idle;

	/** 中心认证与 Match 创建等待超时计时器 */
	FTimerHandle CentralWaitTimer;

	/** 中心就绪等待超时秒数 */
	static constexpr float CentralWaitTimeoutSeconds = 10.0f;

	static constexpr int32 IdlePageIndex = 0;
	static constexpr int32 TeamPageIndex = 1;
	static constexpr int32 OptionsPageIndex = 2;
	static constexpr int32 CodexPageIndex = 3;
};
