// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Online/WeaponCentralConnectivityTypes.h"
#include "WeaponGameState.h"
#include "WeaponPlayerState.h"
#include "WeaponLobbyWidget.generated.h"

class UBorder;
class UButton;
class UHorizontalBox;
class UOverlay;
class UPanelWidget;
class UTextBlock;
class UVerticalBox;
class UWidgetSwitcher;
class UWeaponCentralConnectivitySubsystem;
class UWeaponTalentTreeWidget;

/** 房间 UI 的两种呈现形态：常驻小窗 / 全屏面板。 */
UENUM()
enum class EWeaponLobbyViewState : uint8
{
	/** 屏幕正左常驻小窗，只显示人数与"展开" */
	Collapsed,

	/** 全屏房间面板 */
	Expanded,
};

/** 全屏面板内部的分页。 */
UENUM()
enum class EWeaponLobbyPage : uint8
{
	/** 房间主页：模型预览 + 队伍名单 + 任务简报 + 主操作按钮 */
	Roster,

	/** 任务选择占位页，只有右上返回按钮 */
	MissionSelect,
};

/** 主操作按钮在原型状态机下的三种语义。 */
UENUM()
enum class EWeaponLobbyPrimaryAction : uint8
{
	/** 本地未准备 —— "准备完成" */
	Ready,

	/** 本地已准备，但还不能开局 —— "取消准备" */
	Unready,

	/** 房主已准备且其他人全部就绪 —— "开始游戏" */
	StartGame,
};

/** 联机信息行：标签 + 值文本，行内容由 RefreshSessionInfo 填充。 */
USTRUCT()
struct FWeaponLobbySessionInfoRow
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UTextBlock> Label = nullptr;

	UPROPERTY()
	TObjectPtr<UTextBlock> Value = nullptr;
};

/** 一行队伍名单所需要刷新的控件引用集合。 */
USTRUCT()
struct FWeaponLobbyRosterRow
{
	GENERATED_BODY()

	UPROPERTY()
	TObjectPtr<UBorder> RowBackground = nullptr;

	/** 行首的槽位序号色条，占位时变暗 */
	UPROPERTY()
	TObjectPtr<UBorder> SlotAccent = nullptr;

	UPROPERTY()
	TObjectPtr<UTextBlock> SlotIndexText = nullptr;

	/** 头像占位方块（本次不放职业图标） */
	UPROPERTY()
	TObjectPtr<UBorder> AvatarPlaceholder = nullptr;

	UPROPERTY()
	TObjectPtr<UTextBlock> AvatarHintText = nullptr;

	UPROPERTY()
	TObjectPtr<UTextBlock> PlayerNameText = nullptr;

	/** 玩家信息占位行（职业 / 等级 / 装备等，本次全部为占位文本） */
	UPROPERTY()
	TObjectPtr<UTextBlock> PlayerMetaText = nullptr;

	UPROPERTY()
	TObjectPtr<UBorder> ReadyBadge = nullptr;

	UPROPERTY()
	TObjectPtr<UTextBlock> ReadyBadgeText = nullptr;
};

/**
 *  多人房间 UI（Variant_Weapon）
 *
 *  由 AWeaponPlayerController 在本地客户端创建并常驻，不依赖任何 WBP 资产，
 *  全部用 C++ UMG 原生构建，视觉语言沿用矿业工业科幻：深色半透明 + 橙黄强调。
 *
 *  形态：
 *  - Collapsed：屏幕正左常驻小窗，显示 "1/4" 与"展开"按钮。
 *  - Expanded：全屏房间面板，内部再分 Roster / MissionSelect 两页。
 *
 *  网络：
 *  准备状态走真实网络链路：本地玩家通过 AWeaponPlayerState::RequestSetPlayerReady 提交意图，
 *  服务端校验后写入复制属性，OnLobbyStateChanged / OnMatchPhaseChanged 广播驱动 RefreshLobbyState；
 *  房主且全员就绪时调用 AWeaponGameMode::TryStartWeaponMatch 发起开局倒计时。
 *  房主判定读取本地 PlayerController 的 authority（Listen Host 本地 PC 持有权威），
 *  队友准备状态读取已复制的 PlayerState。0.25s 低频轮询仅兜底远程玩家 join/leave/ready 变化。
 */
UCLASS()
class UWeaponLobbyWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	/** 展开为全屏房间面板；会切回 Roster 页并请求键盘焦点以便 Esc 收起。 */
	UFUNCTION(BlueprintCallable, Category = "Lobby")
	void ExpandLobby();

	/** 收起回屏幕正左的常驻小窗。 */
	UFUNCTION(BlueprintCallable, Category = "Lobby")
	void CollapseLobby();

	/**
	 *  预留刷新入口：重新从 GameState / PlayerState 读取一次并刷新所有文本与配色。
	 *  接网后由 OnLobbyStateChanged / OnMatchPhaseChanged 等复制回调直接调用。
	 */
	UFUNCTION(BlueprintCallable, Category = "Lobby")
	void RefreshLobbyState();

protected:

	//~ UUserWidget
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual FReply NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent) override;

private:

	// ================= 布局构建 =================

	/** 屏幕正左常驻小窗 */
	void BuildCollapsedDock(UPanelWidget* RootCanvas);

	/** 全屏面板外壳：SafeZone + ScaleBox + 设计尺寸画布 + 分页容器 */
	void BuildExpandedShell(UPanelWidget* RootCanvas);

	/** Roster 页（模型预览 / 队伍名单 / 任务简报 / 主操作） */
	void BuildRosterPage(UPanelWidget* StageCanvas);

	/** 任务选择占位页 */
	void BuildMissionSelectPage(UPanelWidget* StageCanvas);

	/** 左上大型模型预览占位框 */
	UWidget* BuildModelPreviewFrame();

	/** 队伍名单区（含右上三个正方形按钮） */
	UWidget* BuildRosterColumn();

	/** 单行队伍名单 */
	UWidget* BuildRosterRow(int32 SlotIndex);

	/** 左下任务简报面板 */
	UWidget* BuildMissionBriefPanel();

	/** 任务简报右侧的主操作按钮 */
	UWidget* BuildPrimaryActionPanel();

	// ================= 小工具 =================

	/** 生成一个统一风格的文本块；LetterSpacing 单位为 1/1000 em。 */
	UTextBlock* MakeText(
		const FString& InText,
		int32 FontSize,
		const FLinearColor& Color,
		bool bBold = false,
		int32 LetterSpacing = 0);

	/** 生成一个已覆盖 Slate 样式的暗色按钮（默认 UButton 是亮灰的，必须覆盖）。 */
	UButton* MakeStyledButton(
		const FLinearColor& NormalColor,
		const FLinearColor& HoveredColor,
		const FLinearColor& PressedColor);

	/** 生成一个纯色面板（含内边距）。 */
	UBorder* MakePanel(const FLinearColor& Color, const FMargin& InPadding);

	/** 生成一条 1px 发丝分隔线。 */
	UWidget* MakeHairline(const FLinearColor& Color, float Thickness = 1.0f);

	/** 生成右上角的三个正方形功能按钮之一。 */
	UWidget* MakeSquareActionButton(const FString& Label, const FString& Glyph, bool bTalentButton = false);

	// ================= 交互 =================

	UFUNCTION()
	void HandleExpandClicked();

	UFUNCTION()
	void HandleCollapseClicked();

	UFUNCTION()
	void HandleMissionSelectClicked();

	UFUNCTION()
	void HandleMissionBackClicked();

	UFUNCTION()
	void HandlePrimaryActionClicked();

	/** 把本地玩家的准备意图提交给网络层（本地写请求函数，服务端校验并广播）。 */
	void RequestLocalReady(bool bNewIsReady);

	/** 占位：装备 / 天赋 / 职业三个入口本次不接实际页面。 */
	UFUNCTION()
	void HandleLoadoutButtonClicked();

	UFUNCTION()
	void HandleTalentButtonClicked();

	// ================= 网络状态 =================

	/** 本地是否为房主：读取 PlayerController 的 authority（Listen Host 的本地 PC 持有权威）。 */
	bool IsLocalHost() const;

	/** 除本地玩家外，其余已连接玩家是否全部准备；没有队友时视为满足。 */
	bool AreOtherPlayersReady() const;

	/** 本地玩家的网络 ready 状态（读复制后的 PlayerState）。 */
	bool IsLocalPlayerReady() const;

	/** 依据准备状态与房主身份解析主操作按钮语义。 */
	EWeaponLobbyPrimaryAction ResolvePrimaryAction() const;

	/** 网络回调：任一 PlayerState 的 ready 变化 / 战局阶段变化后刷新整个面板。 */
	UFUNCTION()
	void HandleLobbyStateChanged(FWeaponPlayerLobbyState OldState, FWeaponPlayerLobbyState NewState);

	UFUNCTION()
	void HandleMatchPhaseChanged(EWeaponMatchPhase NewPhase);

	/** 中心控制面状态变化（握手进度 / Direct/Relay 建立）后刷新联机信息行。 */
	UFUNCTION()
	void HandleCentralConnectivityChanged(EWeaponCentralConnectivityState NewState, FString Message);

	/** 接管对本地 PlayerState 网络事件的监听（重复创建/重建时先解绑旧的）。 */
	void BindLobbyEvents();

	/** 刷新联机信息行（模式 / 角色 / 房间号 / 端口，读 Central 与 World URL）。 */
	void RefreshSessionInfo();

	/** 切换分页 */
	void SetActivePage(EWeaponLobbyPage NewPage);

	// ================= 控件引用 =================

	UPROPERTY()
	TObjectPtr<UWidget> CollapsedRoot = nullptr;

	UPROPERTY()
	TObjectPtr<UWidget> ExpandedRoot = nullptr;

	UPROPERTY()
	TObjectPtr<UWidgetSwitcher> PageSwitcher = nullptr;

	/** 小窗上的 "1/4" */
	UPROPERTY()
	TObjectPtr<UTextBlock> DockCountText = nullptr;

	/** 小窗上的状态副标题 */
	UPROPERTY()
	TObjectPtr<UTextBlock> DockStatusText = nullptr;

	/** 小窗上随准备状态变色的竖条 */
	UPROPERTY()
	TObjectPtr<UBorder> DockAccentBar = nullptr;

	/** 全屏面板顶栏的 "小队 1/4" */
	UPROPERTY()
	TObjectPtr<UTextBlock> HeaderCountText = nullptr;

	UPROPERTY()
	TObjectPtr<UTextBlock> HeaderPhaseText = nullptr;

	/** 小窗上的联机模式标签（LOCAL P2P / CENTRAL） */
	UPROPERTY()
	TObjectPtr<UTextBlock> DockModeText = nullptr;

	/** 全屏面板顶栏下方的联机信息行（模式 / 角色 / 房间号 / 端口） */
	UPROPERTY()
	TObjectPtr<UVerticalBox> SessionInfoBox = nullptr;

	/** 联机信息键值对（标签与值文本），行内容由 RefreshSessionInfo 填充 */
	UPROPERTY()
	TArray<FWeaponLobbySessionInfoRow> SessionInfoRows;

	UPROPERTY()
	TArray<FWeaponLobbyRosterRow> RosterRows;

	UPROPERTY()
	TObjectPtr<UButton> PrimaryActionButton = nullptr;

	UPROPERTY()
	TObjectPtr<UBorder> PrimaryActionSurface = nullptr;

	UPROPERTY()
	TObjectPtr<UTextBlock> PrimaryActionText = nullptr;

	UPROPERTY()
	TObjectPtr<UTextBlock> PrimaryActionHintText = nullptr;

	UPROPERTY()
	TObjectPtr<UWeaponTalentTreeWidget> TalentTreeWidget = nullptr;

	EWeaponLobbyViewState ViewState = EWeaponLobbyViewState::Collapsed;

	EWeaponLobbyPage ActivePage = EWeaponLobbyPage::Roster;

	/** 无网络回调时的兜底轮询计时器 */
	float RefreshAccumulator = 0.0f;

	/** 网络委托已绑定（重复重建时先解绑旧的，避免重复监听） */
	bool bEventsBound = false;

	static constexpr int32 RosterSlotCount = 4;
	static constexpr int32 RosterPageIndex = 0;
	static constexpr int32 MissionPageIndex = 1;
};
