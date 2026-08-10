// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponLobbyWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/BorderSlot.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/SafeZone.h"
#include "Components/ScaleBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "InputCoreTypes.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"
#include "Online/WeaponCentralConnectivitySubsystem.h"
#include "WeaponGameMode.h"

namespace WeaponLobbyStyle
{
	// ---- 色板：深色工业舱室 + 橙黄警示强调 ----

	/** 全屏遮罩：接近黑的冷调，让 3D 场景透出一点点 */
	static const FLinearColor ScrimColor(0.016f, 0.020f, 0.026f, 0.94f);

	/** 一级面板：舱室金属板 */
	static const FLinearColor PanelColor(0.043f, 0.052f, 0.063f, 0.96f);

	/** 二级面板：嵌入式凹槽 */
	static const FLinearColor PanelDeepColor(0.024f, 0.030f, 0.038f, 0.98f);

	/** 三级：抬起的卡片行 */
	static const FLinearColor PanelRaisedColor(0.070f, 0.082f, 0.098f, 0.96f);

	/** 悬停态卡片行 */
	static const FLinearColor PanelHoverColor(0.104f, 0.122f, 0.145f, 0.98f);

	/** 空槽位：几乎没有实体感 */
	static const FLinearColor PanelEmptyColor(0.035f, 0.041f, 0.050f, 0.80f);

	/** 主强调色：安全帽橙黄 */
	static const FLinearColor Amber(1.000f, 0.686f, 0.145f, 1.0f);

	/** 强调色的低亮变体，用于分隔线与边缘 */
	static const FLinearColor AmberDim(1.000f, 0.686f, 0.145f, 0.32f);

	/** 强调色的极低亮变体，用于大面积描边 */
	static const FLinearColor AmberFaint(1.000f, 0.686f, 0.145f, 0.14f);

	/** 已准备：矿灯绿 */
	static const FLinearColor Ready(0.408f, 0.851f, 0.435f, 1.0f);

	/** 主文本 */
	static const FLinearColor TextPrimary(0.925f, 0.941f, 0.957f, 1.0f);

	/** 次文本 */
	static const FLinearColor TextSecondary(0.573f, 0.616f, 0.667f, 1.0f);

	/** 三级文本 / 占位提示 */
	static const FLinearColor TextMuted(0.353f, 0.388f, 0.435f, 1.0f);

	/** 深色文字（压在橙黄底上） */
	static const FLinearColor TextOnAmber(0.055f, 0.047f, 0.031f, 1.0f);

	/** 设计基准分辨率：ScaleBox 会按可用空间等比缩放 */
	static constexpr float DesignWidth = 1760.0f;
	static constexpr float DesignHeight = 940.0f;

	/** 把纯色转成一个可直接塞进 FButtonStyle 的 Brush。
	 *  用 RoundedBox + 0 半径而不是 Box：RoundedBox 由着色器直接出色块，
	 *  不依赖 ResourceObject，避免按钮底色因为没有贴图而不渲染。 */
	FSlateBrush MakeFlatBrush(const FLinearColor& Color)
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.TintColor = FSlateColor(Color);
		Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
		Brush.OutlineSettings.CornerRadii = FVector4(0.0f, 0.0f, 0.0f, 0.0f);
		Brush.OutlineSettings.Width = 0.0f;
		return Brush;
	}

	/** 带描边的 Brush，用于占位框的虚拟"图纸"感 */
	FSlateBrush MakeOutlinedBrush(const FLinearColor& Fill, const FLinearColor& Outline, float Width)
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.TintColor = FSlateColor(Fill);
		Brush.OutlineSettings.Color = FSlateColor(Outline);
		Brush.OutlineSettings.Width = Width;
		Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
		Brush.OutlineSettings.CornerRadii = FVector4(2.0f, 2.0f, 2.0f, 2.0f);
		return Brush;
	}
}

// =====================================================================================
//  生命周期
// =====================================================================================

void UWeaponLobbyWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"));
	}

	UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(GetRootWidget());
	if (!RootCanvas)
	{
		RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();
		WidgetTree->RootWidget = RootCanvas;
	}

	BuildCollapsedDock(RootCanvas);
	BuildExpandedShell(RootCanvas);

	// Esc 收起依赖键盘焦点，全屏面板展开时才请求焦点。
	SetIsFocusable(true);

	BindLobbyEvents();
	CollapseLobby();
	RefreshLobbyState();
}

void UWeaponLobbyWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	// 网络回调只覆盖 ready 与阶段变化；远程玩家中途加入/离开仍以低频轮询兜底，
	// 保证小窗人数与名单能跟上 PlayerArray 的复制。
	RefreshAccumulator += InDeltaTime;
	if (RefreshAccumulator >= 0.25f)
	{
		RefreshAccumulator = 0.0f;
		RefreshLobbyState();
	}
}

void UWeaponLobbyWidget::BindLobbyEvents()
{
	// 每次进入关卡都会重新创建本控件，重复绑定的旧引用已被 GC 或指向旧世界，
	// 先解绑再绑定，避免监听泄漏与重复刷新。
	if (bEventsBound)
	{
		if (AWeaponGameState* OldGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr)
		{
			OldGameState->OnMatchPhaseChanged.RemoveDynamic(this, &UWeaponLobbyWidget::HandleMatchPhaseChanged);
		}
		if (AWeaponPlayerState* OldPlayerState = Cast<AWeaponPlayerState>(GetOwningPlayerState()))
		{
			OldPlayerState->OnLobbyStateChanged.RemoveDynamic(this, &UWeaponLobbyWidget::HandleLobbyStateChanged);
		}
		if (UWeaponCentralConnectivitySubsystem* OldCentral =
			GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponCentralConnectivitySubsystem>() : nullptr)
		{
			OldCentral->OnConnectivityChanged.RemoveDynamic(
				this, &UWeaponLobbyWidget::HandleCentralConnectivityChanged);
		}
		bEventsBound = false;
	}

	if (AWeaponGameState* GameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr)
	{
		GameState->OnMatchPhaseChanged.AddDynamic(this, &UWeaponLobbyWidget::HandleMatchPhaseChanged);
	}
	if (AWeaponPlayerState* PlayerState = Cast<AWeaponPlayerState>(GetOwningPlayerState()))
	{
		PlayerState->OnLobbyStateChanged.AddDynamic(this, &UWeaponLobbyWidget::HandleLobbyStateChanged);
	}
	if (UWeaponCentralConnectivitySubsystem* Central =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponCentralConnectivitySubsystem>() : nullptr)
	{
		Central->OnConnectivityChanged.AddDynamic(this, &UWeaponLobbyWidget::HandleCentralConnectivityChanged);
	}

	bEventsBound = true;
	RefreshLobbyState();
}

void UWeaponLobbyWidget::HandleCentralConnectivityChanged(
	EWeaponCentralConnectivityState NewState,
	FString Message)
{
	RefreshLobbyState();
}

void UWeaponLobbyWidget::HandleLobbyStateChanged(
	FWeaponPlayerLobbyState OldState,
	FWeaponPlayerLobbyState NewState)
{
	RefreshLobbyState();
}

void UWeaponLobbyWidget::HandleMatchPhaseChanged(EWeaponMatchPhase NewPhase)
{
	RefreshLobbyState();
}

FReply UWeaponLobbyWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (ViewState == EWeaponLobbyViewState::Expanded && InKeyEvent.GetKey() == EKeys::Escape)
	{
		// 任务选择页的 Esc 先退回房间主页，再按一次才收起整个面板。
		if (ActivePage == EWeaponLobbyPage::MissionSelect)
		{
			SetActivePage(EWeaponLobbyPage::Roster);
		}
		else
		{
			CollapseLobby();
		}
		return FReply::Handled();
	}

	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

// =====================================================================================
//  形态切换
// =====================================================================================

void UWeaponLobbyWidget::ExpandLobby()
{
	ViewState = EWeaponLobbyViewState::Expanded;

	if (CollapsedRoot)
	{
		CollapsedRoot->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (ExpandedRoot)
	{
		ExpandedRoot->SetVisibility(ESlateVisibility::Visible);
	}

	SetActivePage(EWeaponLobbyPage::Roster);
	RefreshLobbyState();

	// 拿到键盘焦点后 Esc 才能被 NativeOnKeyDown 接住。
	if (APlayerController* PC = GetOwningPlayer())
	{
		SetUserFocus(PC);
	}
}

void UWeaponLobbyWidget::CollapseLobby()
{
	ViewState = EWeaponLobbyViewState::Collapsed;

	if (ExpandedRoot)
	{
		ExpandedRoot->SetVisibility(ESlateVisibility::Collapsed);
	}
	if (CollapsedRoot)
	{
		CollapsedRoot->SetVisibility(ESlateVisibility::Visible);
	}

	RefreshLobbyState();
}

void UWeaponLobbyWidget::SetActivePage(EWeaponLobbyPage NewPage)
{
	ActivePage = NewPage;

	if (PageSwitcher)
	{
		PageSwitcher->SetActiveWidgetIndex(
			NewPage == EWeaponLobbyPage::MissionSelect ? MissionPageIndex : RosterPageIndex);
	}
}

// =====================================================================================
//  常驻小窗（屏幕正左）
// =====================================================================================

void UWeaponLobbyWidget::BuildCollapsedDock(UPanelWidget* RootCanvas)
{
	using namespace WeaponLobbyStyle;

	// 外壳：左边一根橙黄竖条 + 深色主体，贴着屏幕左缘，垂直居中。
	UHorizontalBox* DockRow = WidgetTree->ConstructWidget<UHorizontalBox>();

	// 状态竖条：未准备橙黄 / 已准备绿，是小窗里唯一的高饱和元素。
	UBorder* AccentBar = WidgetTree->ConstructWidget<UBorder>();
	AccentBar->SetBrushColor(Amber);
	AccentBar->SetPadding(FMargin(0.0f));
	DockAccentBar = AccentBar;
	{
		USizeBox* BarSize = WidgetTree->ConstructWidget<USizeBox>();
		BarSize->SetWidthOverride(4.0f);
		AccentBar->AddChild(BarSize);

		UHorizontalBoxSlot* BarSlot = DockRow->AddChildToHorizontalBox(AccentBar);
		BarSlot->SetVerticalAlignment(VAlign_Fill);
	}

	UBorder* DockBody = MakePanel(PanelColor, FMargin(16.0f, 14.0f, 18.0f, 14.0f));
	{
		UHorizontalBoxSlot* BodySlot = DockRow->AddChildToHorizontalBox(DockBody);
		BodySlot->SetVerticalAlignment(VAlign_Fill);
	}

	UVerticalBox* DockContent = WidgetTree->ConstructWidget<UVerticalBox>();
	DockBody->AddChild(DockContent);

	// 顶部小标签
	{
		UTextBlock* Label = MakeText(TEXT("小队"), 10, TextMuted, true, 220);
		UVerticalBoxSlot* LabelSlot = DockContent->AddChildToVerticalBox(Label);
		LabelSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 2.0f));
	}

	// 人数：大号数字，是小窗的信息重心
	UTextBlock* CountText = MakeText(TEXT("1/4"), 30, TextPrimary, true);
	DockCountText = CountText;
	DockContent->AddChildToVerticalBox(CountText);

	// 状态副标题
	UTextBlock* StatusText = MakeText(TEXT("待命"), 11, Amber, false, 120);
	DockStatusText = StatusText;
	{
		UVerticalBoxSlot* StatusSlot = DockContent->AddChildToVerticalBox(StatusText);
		StatusSlot->SetPadding(FMargin(0.0f, 1.0f, 0.0f, 10.0f));
	}

	// 联机模式标签：LOCAL P2P / CENTRAL SERVER，小窗里的最低一级信息
	UTextBlock* ModeText = MakeText(TEXT("LOCAL P2P"), 9, TextMuted, true, 200);
	DockModeText = ModeText;
	{
		UVerticalBoxSlot* ModeSlot = DockContent->AddChildToVerticalBox(ModeText);
		ModeSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 8.0f));
	}

	// 展开按钮：整块可点，宽度撑满小窗
	{
		UButton* ExpandButton = MakeStyledButton(
			FLinearColor(0.129f, 0.145f, 0.169f, 1.0f),
			Amber,
			FLinearColor(0.780f, 0.525f, 0.098f, 1.0f));
		ExpandButton->OnClicked.AddDynamic(this, &UWeaponLobbyWidget::HandleExpandClicked);

		UHorizontalBox* ButtonRow = WidgetTree->ConstructWidget<UHorizontalBox>();

		UTextBlock* ExpandLabel = MakeText(TEXT("展开"), 12, TextPrimary, true, 180);
		UHorizontalBoxSlot* LabelSlot = ButtonRow->AddChildToHorizontalBox(ExpandLabel);
		LabelSlot->SetVerticalAlignment(VAlign_Center);
		LabelSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

		// 右侧箭头暗示"向右打开"
		UTextBlock* Chevron = MakeText(TEXT(">"), 12, Amber, true);
		UHorizontalBoxSlot* ChevronSlot = ButtonRow->AddChildToHorizontalBox(Chevron);
		ChevronSlot->SetVerticalAlignment(VAlign_Center);
		ChevronSlot->SetPadding(FMargin(10.0f, 0.0f, 0.0f, 0.0f));

		ExpandButton->AddChild(ButtonRow);
		if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(ExpandButton->GetContentSlot()))
		{
			ContentSlot->SetPadding(FMargin(12.0f, 7.0f));
			ContentSlot->SetHorizontalAlignment(HAlign_Fill);
		}

		DockContent->AddChildToVerticalBox(ExpandButton);
	}

	// 挂到画布：左缘垂直居中，锚点 (0, 0.5) 保证任意分辨率都贴正左。
	CollapsedRoot = DockRow;
	UCanvasPanelSlot* DockSlot = Cast<UCanvasPanelSlot>(RootCanvas->AddChild(DockRow));
	DockSlot->SetAnchors(FAnchors(0.0f, 0.5f, 0.0f, 0.5f));
	DockSlot->SetAlignment(FVector2D(0.0f, 0.5f));
	DockSlot->SetAutoSize(true);
	DockSlot->SetPosition(FVector2D(0.0f, 0.0f));
}

// =====================================================================================
//  全屏面板外壳
// =====================================================================================

void UWeaponLobbyWidget::BuildExpandedShell(UPanelWidget* RootCanvas)
{
	using namespace WeaponLobbyStyle;

	// 全屏遮罩 + 内容层
	UOverlay* ShellOverlay = WidgetTree->ConstructWidget<UOverlay>();

	// 第 0 层：深色遮罩，吃掉背后场景的对比度
	{
		UBorder* Scrim = WidgetTree->ConstructWidget<UBorder>();
		Scrim->SetBrushColor(ScrimColor);
		UOverlaySlot* ScrimSlot = ShellOverlay->AddChildToOverlay(Scrim);
		ScrimSlot->SetHorizontalAlignment(HAlign_Fill);
		ScrimSlot->SetVerticalAlignment(VAlign_Fill);
	}

	// 第 1 层：SafeZone -> ScaleBox -> 设计尺寸 -> 内容
	USafeZone* SafeArea = WidgetTree->ConstructWidget<USafeZone>();
	{
		UOverlaySlot* SafeSlot = ShellOverlay->AddChildToOverlay(SafeArea);
		SafeSlot->SetHorizontalAlignment(HAlign_Fill);
		SafeSlot->SetVerticalAlignment(VAlign_Fill);
		SafeSlot->SetPadding(FMargin(28.0f, 22.0f));
	}

	UScaleBox* Scaler = WidgetTree->ConstructWidget<UScaleBox>();
	Scaler->SetStretch(EStretch::ScaleToFit);
	SafeArea->AddChild(Scaler);

	USizeBox* DesignBox = WidgetTree->ConstructWidget<USizeBox>();
	DesignBox->SetWidthOverride(DesignWidth);
	DesignBox->SetHeightOverride(DesignHeight);
	Scaler->AddChild(DesignBox);

	UVerticalBox* ShellColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	DesignBox->AddChild(ShellColumn);

	// ---- 顶栏：标题 / 人数 / 阶段 / 收起 ----
	{
		UHorizontalBox* HeaderRow = WidgetTree->ConstructWidget<UHorizontalBox>();

		// 左侧橙黄竖条 + 标题组，形成一个明确的"起点"
		{
			UBorder* TitleAccent = WidgetTree->ConstructWidget<UBorder>();
			TitleAccent->SetBrushColor(Amber);
			USizeBox* AccentSize = WidgetTree->ConstructWidget<USizeBox>();
			AccentSize->SetWidthOverride(5.0f);
			AccentSize->SetHeightOverride(38.0f);
			TitleAccent->AddChild(AccentSize);

			UHorizontalBoxSlot* AccentSlot = HeaderRow->AddChildToHorizontalBox(TitleAccent);
			AccentSlot->SetVerticalAlignment(VAlign_Center);
			AccentSlot->SetPadding(FMargin(0.0f, 0.0f, 14.0f, 0.0f));
		}

		{
			UVerticalBox* TitleColumn = WidgetTree->ConstructWidget<UVerticalBox>();
			TitleColumn->AddChildToVerticalBox(MakeText(TEXT("任务准备室"), 26, TextPrimary, true, 60));

			UTextBlock* SubTitle = MakeText(TEXT("DEEP ROCK · MISSION CONTROL"), 10, TextMuted, false, 300);
			UVerticalBoxSlot* SubSlot = TitleColumn->AddChildToVerticalBox(SubTitle);
			SubSlot->SetPadding(FMargin(0.0f, 2.0f, 0.0f, 0.0f));

			UHorizontalBoxSlot* ColumnSlot = HeaderRow->AddChildToHorizontalBox(TitleColumn);
			ColumnSlot->SetVerticalAlignment(VAlign_Center);
		}

		// 中间弹性留白
		{
			USpacer* Gap = WidgetTree->ConstructWidget<USpacer>();
			UHorizontalBoxSlot* GapSlot = HeaderRow->AddChildToHorizontalBox(Gap);
			GapSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}

		// 阶段标签
		{
			UTextBlock* PhaseText = MakeText(TEXT("待命中"), 11, TextSecondary, false, 200);
			HeaderPhaseText = PhaseText;
			UHorizontalBoxSlot* PhaseSlot = HeaderRow->AddChildToHorizontalBox(PhaseText);
			PhaseSlot->SetVerticalAlignment(VAlign_Center);
			PhaseSlot->SetPadding(FMargin(0.0f, 0.0f, 20.0f, 0.0f));
		}

		// 人数
		{
			UBorder* CountChip = MakePanel(PanelDeepColor, FMargin(16.0f, 8.0f));
			UTextBlock* CountText = MakeText(TEXT("小队 1/4"), 14, Amber, true, 100);
			HeaderCountText = CountText;
			CountChip->AddChild(CountText);

			UHorizontalBoxSlot* ChipSlot = HeaderRow->AddChildToHorizontalBox(CountChip);
			ChipSlot->SetVerticalAlignment(VAlign_Center);
			ChipSlot->SetPadding(FMargin(0.0f, 0.0f, 14.0f, 0.0f));
		}

		// 收起按钮：明确的关闭控件，并标注 Esc
		{
			UButton* CollapseButton = MakeStyledButton(
				FLinearColor(0.106f, 0.078f, 0.078f, 1.0f),
				FLinearColor(0.694f, 0.216f, 0.196f, 1.0f),
				FLinearColor(0.490f, 0.145f, 0.133f, 1.0f));
			CollapseButton->OnClicked.AddDynamic(this, &UWeaponLobbyWidget::HandleCollapseClicked);

			UHorizontalBox* CollapseRow = WidgetTree->ConstructWidget<UHorizontalBox>();

			UTextBlock* CollapseLabel = MakeText(TEXT("收起"), 12, TextPrimary, true, 140);
			UHorizontalBoxSlot* LabelSlot = CollapseRow->AddChildToHorizontalBox(CollapseLabel);
			LabelSlot->SetVerticalAlignment(VAlign_Center);

			UTextBlock* EscHint = MakeText(TEXT("ESC"), 9, TextMuted, true, 200);
			UHorizontalBoxSlot* HintSlot = CollapseRow->AddChildToHorizontalBox(EscHint);
			HintSlot->SetVerticalAlignment(VAlign_Center);
			HintSlot->SetPadding(FMargin(10.0f, 0.0f, 0.0f, 0.0f));

			CollapseButton->AddChild(CollapseRow);
			if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(CollapseButton->GetContentSlot()))
			{
				ContentSlot->SetPadding(FMargin(18.0f, 10.0f));
			}

			UHorizontalBoxSlot* ButtonSlot = HeaderRow->AddChildToHorizontalBox(CollapseButton);
			ButtonSlot->SetVerticalAlignment(VAlign_Center);
		}

		UVerticalBoxSlot* HeaderSlot = ShellColumn->AddChildToVerticalBox(HeaderRow);
		HeaderSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 10.0f));
	}

	// 顶栏下的橙黄发丝线，把标题和内容切开
	{
		UVerticalBoxSlot* LineSlot = ShellColumn->AddChildToVerticalBox(MakeHairline(AmberDim, 2.0f));
		LineSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 12.0f));
	}

	// ---- 联机信息行：模式 / 角色 / 房间号 / 端口（房主多显示端口）----
	{
		UBorder* InfoPanel = MakePanel(PanelDeepColor, FMargin(16.0f, 10.0f));
		UVerticalBox* InfoColumn = WidgetTree->ConstructWidget<UVerticalBox>();
		InfoPanel->AddChild(InfoColumn);
		SessionInfoBox = InfoColumn;

		// 三行小号键值对：模式 / 角色 / 网络地址与房间
		const TCHAR* RowLabels[] = { TEXT("模式"), TEXT("角色"), TEXT("房间") };
		for (int32 RowIndex = 0; RowIndex < UE_ARRAY_COUNT(RowLabels); ++RowIndex)
		{
			UTextBlock* Label = MakeText(RowLabels[RowIndex], 10, TextMuted, true, 200);
			UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
			UHorizontalBoxSlot* LabelSlot = Row->AddChildToHorizontalBox(Label);
			LabelSlot->SetVerticalAlignment(VAlign_Center);
			LabelSlot->SetPadding(FMargin(0.0f, 0.0f, 16.0f, 0.0f));

			UTextBlock* Value = MakeText(TEXT("--"), 11, TextPrimary, false, 80);
			Value->SetAutoWrapText(true);
			UHorizontalBoxSlot* ValueSlot = Row->AddChildToHorizontalBox(Value);
			ValueSlot->SetVerticalAlignment(VAlign_Center);
			ValueSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

			UVerticalBoxSlot* RowSlot = InfoColumn->AddChildToVerticalBox(Row);
			RowSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, RowIndex < 2 ? 4.0f : 0.0f));
			SessionInfoRows.Add(FWeaponLobbySessionInfoRow{ Label, Value });
		}

		UVerticalBoxSlot* InfoSlot = ShellColumn->AddChildToVerticalBox(InfoPanel);
		InfoSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 16.0f));
	}

	// ---- 内容区：两页切换 ----
	UWidgetSwitcher* Switcher = WidgetTree->ConstructWidget<UWidgetSwitcher>();
	PageSwitcher = Switcher;
	{
		UVerticalBoxSlot* SwitcherSlot = ShellColumn->AddChildToVerticalBox(Switcher);
		SwitcherSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		SwitcherSlot->SetHorizontalAlignment(HAlign_Fill);
		SwitcherSlot->SetVerticalAlignment(VAlign_Fill);
	}

	BuildRosterPage(Switcher);
	BuildMissionSelectPage(Switcher);
	Switcher->SetActiveWidgetIndex(RosterPageIndex);

	ExpandedRoot = ShellOverlay;
	UCanvasPanelSlot* ShellSlot = Cast<UCanvasPanelSlot>(RootCanvas->AddChild(ShellOverlay));
	ShellSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
	ShellSlot->SetOffsets(FMargin(0.0f));
}

// =====================================================================================
//  Roster 页
// =====================================================================================

void UWeaponLobbyWidget::BuildRosterPage(UPanelWidget* StageCanvas)
{
	using namespace WeaponLobbyStyle;

	// 上下两段：上段模型预览 + 名单，下段任务简报 + 主操作。
	UVerticalBox* PageColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	StageCanvas->AddChild(PageColumn);

	// ---- 上段 ----
	{
		UHorizontalBox* UpperRow = WidgetTree->ConstructWidget<UHorizontalBox>();

		// 左：大型模型预览框，尽量靠左，占据视觉主体
		// Automatic = 只取内部 SizeBox 声明的 560px，不参与横向拉伸，
		// 剩余宽度全部让给右侧名单。
		{
			UHorizontalBoxSlot* PreviewSlot = UpperRow->AddChildToHorizontalBox(BuildModelPreviewFrame());
			PreviewSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
			PreviewSlot->SetVerticalAlignment(VAlign_Fill);
			PreviewSlot->SetPadding(FMargin(0.0f, 0.0f, 20.0f, 0.0f));
		}

		// 右：紧贴预览框的队伍名单
		{
			UHorizontalBoxSlot* RosterSlot = UpperRow->AddChildToHorizontalBox(BuildRosterColumn());
			RosterSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			RosterSlot->SetVerticalAlignment(VAlign_Fill);
		}

		UVerticalBoxSlot* UpperSlot = PageColumn->AddChildToVerticalBox(UpperRow);
		UpperSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		UpperSlot->SetHorizontalAlignment(HAlign_Fill);
		UpperSlot->SetVerticalAlignment(VAlign_Fill);
		UpperSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 20.0f));
	}

	// ---- 下段 ----
	{
		UHorizontalBox* LowerRow = WidgetTree->ConstructWidget<UHorizontalBox>();

		// 左下：任务简报
		{
			UHorizontalBoxSlot* BriefSlot = LowerRow->AddChildToHorizontalBox(BuildMissionBriefPanel());
			BriefSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			BriefSlot->SetVerticalAlignment(VAlign_Fill);
			BriefSlot->SetPadding(FMargin(0.0f, 0.0f, 20.0f, 0.0f));
		}

		// 右：主操作按钮
		// Automatic = 只取内部 SizeBox 声明的 300px，剩余宽度归左侧任务简报。
		{
			UHorizontalBoxSlot* ActionSlot = LowerRow->AddChildToHorizontalBox(BuildPrimaryActionPanel());
			ActionSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
			ActionSlot->SetVerticalAlignment(VAlign_Fill);
		}

		// 下段整体高度由内容决定，剩余纵向空间留给上段。
		UVerticalBoxSlot* LowerSlot = PageColumn->AddChildToVerticalBox(LowerRow);
		LowerSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
		LowerSlot->SetHorizontalAlignment(HAlign_Fill);
	}
}

UWidget* UWeaponLobbyWidget::BuildModelPreviewFrame()
{
	using namespace WeaponLobbyStyle;

	// 固定设计宽度，把它钉在整页最左侧，形成"角色站在舱室里"的主视觉位。
	USizeBox* FrameSize = WidgetTree->ConstructWidget<USizeBox>();
	FrameSize->SetWidthOverride(560.0f);

	UBorder* Frame = WidgetTree->ConstructWidget<UBorder>();
	Frame->SetBrush(MakeOutlinedBrush(PanelDeepColor, AmberFaint, 1.0f));
	Frame->SetPadding(FMargin(0.0f));
	FrameSize->AddChild(Frame);

	// 内部三段：标签条 / 预览留空 / 底部刻度信息
	UVerticalBox* FrameColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	Frame->AddChild(FrameColumn);

	// 顶部标签条
	{
		UBorder* TagBar = MakePanel(FLinearColor(0.086f, 0.098f, 0.114f, 1.0f), FMargin(16.0f, 9.0f));
		UHorizontalBox* TagRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		TagBar->AddChild(TagRow);

		UTextBlock* Tag = MakeText(TEXT("装备预览"), 11, Amber, true, 240);
		UHorizontalBoxSlot* TagSlot = TagRow->AddChildToHorizontalBox(Tag);
		TagSlot->SetVerticalAlignment(VAlign_Center);
		TagSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

		UTextBlock* Code = MakeText(TEXT("RIG-01"), 10, TextMuted, false, 200);
		UHorizontalBoxSlot* CodeSlot = TagRow->AddChildToHorizontalBox(Code);
		CodeSlot->SetVerticalAlignment(VAlign_Center);

		FrameColumn->AddChildToVerticalBox(TagBar);
	}

	// 中部：真正的 3D 预览未来会挂在这里，现在留一个居中的占位标识
	{
		UOverlay* Stage = WidgetTree->ConstructWidget<UOverlay>();

		// 背景：比外框更深一档，制造凹陷
		{
			UBorder* StageBack = WidgetTree->ConstructWidget<UBorder>();
			StageBack->SetBrushColor(FLinearColor(0.016f, 0.020f, 0.026f, 1.0f));
			UOverlaySlot* BackSlot = Stage->AddChildToOverlay(StageBack);
			BackSlot->SetHorizontalAlignment(HAlign_Fill);
			BackSlot->SetVerticalAlignment(VAlign_Fill);
		}

		// 中央占位：一个细线方框 + 说明文字
		{
			UVerticalBox* Hint = WidgetTree->ConstructWidget<UVerticalBox>();

			UBorder* Glyph = WidgetTree->ConstructWidget<UBorder>();
			Glyph->SetBrush(MakeOutlinedBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.02f), AmberDim, 1.0f));
			{
				USizeBox* GlyphSize = WidgetTree->ConstructWidget<USizeBox>();
				GlyphSize->SetWidthOverride(150.0f);
				GlyphSize->SetHeightOverride(240.0f);
				Glyph->AddChild(GlyphSize);
			}
			UVerticalBoxSlot* GlyphSlot = Hint->AddChildToVerticalBox(Glyph);
			GlyphSlot->SetHorizontalAlignment(HAlign_Center);
			GlyphSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 18.0f));

			UTextBlock* HintText = MakeText(TEXT("MODEL PREVIEW"), 11, TextMuted, true, 340);
			HintText->SetJustification(ETextJustify::Center);
			UVerticalBoxSlot* HintSlot = Hint->AddChildToVerticalBox(HintText);
			HintSlot->SetHorizontalAlignment(HAlign_Center);

			UTextBlock* SubText = MakeText(TEXT("角色模型占位"), 10, TextMuted, false, 160);
			SubText->SetJustification(ETextJustify::Center);
			UVerticalBoxSlot* SubSlot = Hint->AddChildToVerticalBox(SubText);
			SubSlot->SetHorizontalAlignment(HAlign_Center);
			SubSlot->SetPadding(FMargin(0.0f, 4.0f, 0.0f, 0.0f));

			UOverlaySlot* HintOverlaySlot = Stage->AddChildToOverlay(Hint);
			HintOverlaySlot->SetHorizontalAlignment(HAlign_Center);
			HintOverlaySlot->SetVerticalAlignment(VAlign_Center);
		}

		UVerticalBoxSlot* StageSlot = FrameColumn->AddChildToVerticalBox(Stage);
		StageSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		StageSlot->SetHorizontalAlignment(HAlign_Fill);
		StageSlot->SetVerticalAlignment(VAlign_Fill);
	}

	// 底部刻度信息条
	{
		UBorder* FootBar = MakePanel(FLinearColor(0.086f, 0.098f, 0.114f, 1.0f), FMargin(16.0f, 8.0f));
		UHorizontalBox* FootRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		FootBar->AddChild(FootRow);

		const TCHAR* FootLabels[] = { TEXT("装备 --"), TEXT("天赋 --"), TEXT("职业 --") };
		for (int32 Index = 0; Index < UE_ARRAY_COUNT(FootLabels); ++Index)
		{
			UTextBlock* Item = MakeText(FootLabels[Index], 10, TextMuted, false, 100);
			UHorizontalBoxSlot* ItemSlot = FootRow->AddChildToHorizontalBox(Item);
			ItemSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			ItemSlot->SetVerticalAlignment(VAlign_Center);
		}

		FrameColumn->AddChildToVerticalBox(FootBar);
	}

	return FrameSize;
}

UWidget* UWeaponLobbyWidget::BuildRosterColumn()
{
	using namespace WeaponLobbyStyle;

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();

	// ---- 名单标题行 + 右上三个正方形按钮 ----
	{
		UHorizontalBox* TitleRow = WidgetTree->ConstructWidget<UHorizontalBox>();

		{
			UVerticalBox* TitleColumn = WidgetTree->ConstructWidget<UVerticalBox>();
			TitleColumn->AddChildToVerticalBox(MakeText(TEXT("小队名单"), 17, TextPrimary, true, 80));

			UTextBlock* Sub = MakeText(TEXT("SQUAD ROSTER"), 9, TextMuted, false, 320);
			UVerticalBoxSlot* SubSlot = TitleColumn->AddChildToVerticalBox(Sub);
			SubSlot->SetPadding(FMargin(0.0f, 2.0f, 0.0f, 0.0f));

			UHorizontalBoxSlot* TitleSlot = TitleRow->AddChildToHorizontalBox(TitleColumn);
			TitleSlot->SetVerticalAlignment(VAlign_Bottom);
			TitleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}

		// 三个正方形按钮：一行，右对齐顶部
		{
			UHorizontalBox* ActionRow = WidgetTree->ConstructWidget<UHorizontalBox>();

			struct FSquareSpec
			{
				const TCHAR* Label;
				const TCHAR* Glyph;
			};

			const FSquareSpec Specs[] =
			{
				{ TEXT("选择装备"), TEXT("装") },
				{ TEXT("选择天赋"), TEXT("赋") },
				{ TEXT("选择职业"), TEXT("职") },
			};

			for (int32 Index = 0; Index < UE_ARRAY_COUNT(Specs); ++Index)
			{
				UHorizontalBoxSlot* ButtonSlot =
					ActionRow->AddChildToHorizontalBox(MakeSquareActionButton(Specs[Index].Label, Specs[Index].Glyph));
				ButtonSlot->SetPadding(FMargin(Index == 0 ? 0.0f : 10.0f, 0.0f, 0.0f, 0.0f));
				ButtonSlot->SetVerticalAlignment(VAlign_Top);
			}

			UHorizontalBoxSlot* ActionSlot = TitleRow->AddChildToHorizontalBox(ActionRow);
			ActionSlot->SetHorizontalAlignment(HAlign_Right);
			ActionSlot->SetVerticalAlignment(VAlign_Top);
		}

		UVerticalBoxSlot* TitleRowSlot = Column->AddChildToVerticalBox(TitleRow);
		TitleRowSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 12.0f));
	}

	// ---- 四行名单 ----
	RosterRows.Reset();
	for (int32 SlotIndex = 0; SlotIndex < RosterSlotCount; ++SlotIndex)
	{
		UVerticalBoxSlot* RowSlot = Column->AddChildToVerticalBox(BuildRosterRow(SlotIndex));
		RowSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 8.0f));
		RowSlot->SetHorizontalAlignment(HAlign_Fill);
	}

	// 名单不足四行时下方留白，避免行被拉伸变形
	{
		USpacer* Tail = WidgetTree->ConstructWidget<USpacer>();
		UVerticalBoxSlot* TailSlot = Column->AddChildToVerticalBox(Tail);
		TailSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	return Column;
}

UWidget* UWeaponLobbyWidget::BuildRosterRow(int32 SlotIndex)
{
	using namespace WeaponLobbyStyle;

	FWeaponLobbyRosterRow Row;

	UBorder* RowBackground = WidgetTree->ConstructWidget<UBorder>();
	RowBackground->SetBrushColor(PanelEmptyColor);
	RowBackground->SetPadding(FMargin(0.0f));
	Row.RowBackground = RowBackground;

	UHorizontalBox* RowBox = WidgetTree->ConstructWidget<UHorizontalBox>();
	RowBackground->AddChild(RowBox);

	// 行首色条：被占用时点亮，空位时保持暗色
	{
		UBorder* Accent = WidgetTree->ConstructWidget<UBorder>();
		Accent->SetBrushColor(AmberFaint);
		Row.SlotAccent = Accent;

		USizeBox* AccentSize = WidgetTree->ConstructWidget<USizeBox>();
		AccentSize->SetWidthOverride(4.0f);
		Accent->AddChild(AccentSize);

		UHorizontalBoxSlot* AccentSlot = RowBox->AddChildToHorizontalBox(Accent);
		AccentSlot->SetVerticalAlignment(VAlign_Fill);
	}

	// 槽位序号
	{
		UTextBlock* IndexText = MakeText(FString::Printf(TEXT("%02d"), SlotIndex + 1), 13, TextMuted, true, 100);
		Row.SlotIndexText = IndexText;
		UHorizontalBoxSlot* IndexSlot = RowBox->AddChildToHorizontalBox(IndexText);
		IndexSlot->SetVerticalAlignment(VAlign_Center);
		IndexSlot->SetPadding(FMargin(14.0f, 0.0f, 12.0f, 0.0f));
	}

	// 头像占位：一个正方形细线框，不放任何职业图标
	{
		UBorder* AvatarFrame = WidgetTree->ConstructWidget<UBorder>();
		AvatarFrame->SetBrush(MakeOutlinedBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.03f), AmberFaint, 1.0f));
		AvatarFrame->SetPadding(FMargin(0.0f));
		Row.AvatarPlaceholder = AvatarFrame;

		USizeBox* AvatarSize = WidgetTree->ConstructWidget<USizeBox>();
		AvatarSize->SetWidthOverride(56.0f);
		AvatarSize->SetHeightOverride(56.0f);
		AvatarFrame->AddChild(AvatarSize);

		UTextBlock* AvatarHint = MakeText(TEXT("矿工"), 9, TextMuted, false, 120);
		AvatarHint->SetJustification(ETextJustify::Center);
		Row.AvatarHintText = AvatarHint;
		AvatarSize->AddChild(AvatarHint);

		UHorizontalBoxSlot* AvatarSlot = RowBox->AddChildToHorizontalBox(AvatarFrame);
		AvatarSlot->SetVerticalAlignment(VAlign_Center);
		AvatarSlot->SetPadding(FMargin(0.0f, 12.0f, 14.0f, 12.0f));
	}

	// 玩家名 + 信息占位（本次不显示真实玩家名，统一用"矿工 0N"占位标识）
	{
		UVerticalBox* InfoColumn = WidgetTree->ConstructWidget<UVerticalBox>();

		UTextBlock* NameText = MakeText(FString::Printf(TEXT("矿工 %02d"), SlotIndex + 1), 15, TextMuted, true, 40);
		Row.PlayerNameText = NameText;
		InfoColumn->AddChildToVerticalBox(NameText);

		UTextBlock* MetaText = MakeText(TEXT("等待矿工接入 —"), 10, TextMuted, false, 60);
		Row.PlayerMetaText = MetaText;
		UVerticalBoxSlot* MetaSlot = InfoColumn->AddChildToVerticalBox(MetaText);
		MetaSlot->SetPadding(FMargin(0.0f, 4.0f, 0.0f, 0.0f));

		UHorizontalBoxSlot* InfoSlot = RowBox->AddChildToHorizontalBox(InfoColumn);
		InfoSlot->SetVerticalAlignment(VAlign_Center);
		InfoSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	// 准备状态徽章
	{
		UBorder* Badge = MakePanel(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f), FMargin(12.0f, 6.0f));
		Row.ReadyBadge = Badge;

		UTextBlock* BadgeText = MakeText(TEXT("—"), 10, TextMuted, true, 180);
		BadgeText->SetJustification(ETextJustify::Center);
		Row.ReadyBadgeText = BadgeText;
		Badge->AddChild(BadgeText);

		UHorizontalBoxSlot* BadgeSlot = RowBox->AddChildToHorizontalBox(Badge);
		BadgeSlot->SetVerticalAlignment(VAlign_Center);
		BadgeSlot->SetPadding(FMargin(12.0f, 0.0f, 14.0f, 0.0f));
	}

	RosterRows.Add(Row);
	return RowBackground;
}

UWidget* UWeaponLobbyWidget::BuildMissionBriefPanel()
{
	using namespace WeaponLobbyStyle;

	// 参考深岩银河大厅：左侧任务 icon，右侧任务名 + 若干参数标签，底部一个"任务选择"入口。
	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>();
	Panel->SetBrush(MakeOutlinedBrush(PanelColor, AmberFaint, 1.0f));
	Panel->SetPadding(FMargin(20.0f, 18.0f));

	UVerticalBox* PanelColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	Panel->AddChild(PanelColumn);

	// 小标题
	{
		UTextBlock* Label = MakeText(TEXT("当前任务"), 10, Amber, true, 300);
		UVerticalBoxSlot* LabelSlot = PanelColumn->AddChildToVerticalBox(Label);
		LabelSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 12.0f));
	}

	// 主体：icon + 文本
	{
		UHorizontalBox* BriefRow = WidgetTree->ConstructWidget<UHorizontalBox>();

		// 任务 icon 占位
		{
			UBorder* IconFrame = WidgetTree->ConstructWidget<UBorder>();
			IconFrame->SetBrush(MakeOutlinedBrush(PanelDeepColor, AmberDim, 1.0f));
			IconFrame->SetPadding(FMargin(0.0f));

			USizeBox* IconSize = WidgetTree->ConstructWidget<USizeBox>();
			IconSize->SetWidthOverride(84.0f);
			IconSize->SetHeightOverride(84.0f);
			IconFrame->AddChild(IconSize);

			UTextBlock* IconHint = MakeText(TEXT("任务\nICON"), 9, TextMuted, true, 140);
			IconHint->SetJustification(ETextJustify::Center);
			IconSize->AddChild(IconHint);

			UHorizontalBoxSlot* IconSlot = BriefRow->AddChildToHorizontalBox(IconFrame);
			IconSlot->SetVerticalAlignment(VAlign_Center);
			IconSlot->SetPadding(FMargin(0.0f, 0.0f, 18.0f, 0.0f));
		}

		// 任务名 + 参数
		{
			UVerticalBox* TextColumn = WidgetTree->ConstructWidget<UVerticalBox>();

			TextColumn->AddChildToVerticalBox(MakeText(TEXT("未选择任务"), 20, TextPrimary, true, 40));

			UTextBlock* Desc = MakeText(TEXT("从任务终端选择一个作业区域后，此处显示目标与危险等级。"), 11, TextSecondary);
			Desc->SetAutoWrapText(true);
			UVerticalBoxSlot* DescSlot = TextColumn->AddChildToVerticalBox(Desc);
			DescSlot->SetPadding(FMargin(0.0f, 6.0f, 0.0f, 12.0f));

			// 参数标签行
			{
				UHorizontalBox* ChipRow = WidgetTree->ConstructWidget<UHorizontalBox>();

				const TCHAR* ChipLabels[] = { TEXT("区域  --"), TEXT("目标  --"), TEXT("危险度  --"), TEXT("时长  --") };
				for (int32 Index = 0; Index < UE_ARRAY_COUNT(ChipLabels); ++Index)
				{
					UBorder* Chip = MakePanel(PanelDeepColor, FMargin(12.0f, 6.0f));
					Chip->AddChild(MakeText(ChipLabels[Index], 10, TextMuted, false, 80));

					UHorizontalBoxSlot* ChipSlot = ChipRow->AddChildToHorizontalBox(Chip);
					ChipSlot->SetPadding(FMargin(Index == 0 ? 0.0f : 8.0f, 0.0f, 0.0f, 0.0f));
					ChipSlot->SetVerticalAlignment(VAlign_Center);
				}

				TextColumn->AddChildToVerticalBox(ChipRow);
			}

			UHorizontalBoxSlot* TextSlot = BriefRow->AddChildToHorizontalBox(TextColumn);
			TextSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			TextSlot->SetVerticalAlignment(VAlign_Center);
		}

		// 任务选择入口：贴在简报右端，垂直居中
		{
			UButton* MissionButton = MakeStyledButton(
				FLinearColor(0.129f, 0.145f, 0.169f, 1.0f),
				Amber,
				FLinearColor(0.780f, 0.525f, 0.098f, 1.0f));
			MissionButton->OnClicked.AddDynamic(this, &UWeaponLobbyWidget::HandleMissionSelectClicked);

			UVerticalBox* ButtonColumn = WidgetTree->ConstructWidget<UVerticalBox>();

			UTextBlock* MainLabel = MakeText(TEXT("任务选择"), 13, TextPrimary, true, 120);
			MainLabel->SetJustification(ETextJustify::Center);
			ButtonColumn->AddChildToVerticalBox(MainLabel);

			UTextBlock* SubLabel = MakeText(TEXT("MISSION TERMINAL"), 8, TextMuted, false, 240);
			SubLabel->SetJustification(ETextJustify::Center);
			UVerticalBoxSlot* SubSlot = ButtonColumn->AddChildToVerticalBox(SubLabel);
			SubSlot->SetPadding(FMargin(0.0f, 3.0f, 0.0f, 0.0f));
			SubSlot->SetHorizontalAlignment(HAlign_Center);

			MissionButton->AddChild(ButtonColumn);
			if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(MissionButton->GetContentSlot()))
			{
				ContentSlot->SetPadding(FMargin(22.0f, 14.0f));
			}

			UHorizontalBoxSlot* ButtonSlot = BriefRow->AddChildToHorizontalBox(MissionButton);
			ButtonSlot->SetVerticalAlignment(VAlign_Center);
			ButtonSlot->SetPadding(FMargin(20.0f, 0.0f, 0.0f, 0.0f));
		}

		PanelColumn->AddChildToVerticalBox(BriefRow);
	}

	return Panel;
}

UWidget* UWeaponLobbyWidget::BuildPrimaryActionPanel()
{
	using namespace WeaponLobbyStyle;

	// 整个下段右端只有这一个按钮，尺寸和配色都要压过其他控件。
	USizeBox* ActionSize = WidgetTree->ConstructWidget<USizeBox>();
	ActionSize->SetWidthOverride(300.0f);

	UButton* ActionButton = MakeStyledButton(
		FLinearColor(0.0f, 0.0f, 0.0f, 0.0f),
		FLinearColor(1.0f, 1.0f, 1.0f, 0.06f),
		FLinearColor(0.0f, 0.0f, 0.0f, 0.14f));
	ActionButton->OnClicked.AddDynamic(this, &UWeaponLobbyWidget::HandlePrimaryActionClicked);
	PrimaryActionButton = ActionButton;
	ActionSize->AddChild(ActionButton);

	// 按钮底色单独放在一层 Border 上，方便按状态切换填充色。
	UBorder* ActionSurface = WidgetTree->ConstructWidget<UBorder>();
	ActionSurface->SetBrushColor(Amber);
	ActionSurface->SetPadding(FMargin(24.0f, 22.0f));
	PrimaryActionSurface = ActionSurface;
	ActionButton->AddChild(ActionSurface);
	if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(ActionButton->GetContentSlot()))
	{
		ContentSlot->SetPadding(FMargin(0.0f));
		ContentSlot->SetHorizontalAlignment(HAlign_Fill);
		ContentSlot->SetVerticalAlignment(VAlign_Fill);
	}

	UVerticalBox* ActionColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	ActionSurface->AddChild(ActionColumn);

	{
		USpacer* TopGap = WidgetTree->ConstructWidget<USpacer>();
		UVerticalBoxSlot* GapSlot = ActionColumn->AddChildToVerticalBox(TopGap);
		GapSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	UTextBlock* ActionText = MakeText(TEXT("准备完成"), 24, TextOnAmber, true, 100);
	ActionText->SetJustification(ETextJustify::Center);
	PrimaryActionText = ActionText;
	{
		UVerticalBoxSlot* TextSlot = ActionColumn->AddChildToVerticalBox(ActionText);
		TextSlot->SetHorizontalAlignment(HAlign_Center);
	}

	UTextBlock* ActionHint = MakeText(TEXT("READY UP"), 9, FLinearColor(0.055f, 0.047f, 0.031f, 0.65f), true, 340);
	ActionHint->SetJustification(ETextJustify::Center);
	PrimaryActionHintText = ActionHint;
	{
		UVerticalBoxSlot* HintSlot = ActionColumn->AddChildToVerticalBox(ActionHint);
		HintSlot->SetHorizontalAlignment(HAlign_Center);
		HintSlot->SetPadding(FMargin(0.0f, 5.0f, 0.0f, 0.0f));
	}

	{
		USpacer* BottomGap = WidgetTree->ConstructWidget<USpacer>();
		UVerticalBoxSlot* GapSlot = ActionColumn->AddChildToVerticalBox(BottomGap);
		GapSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	return ActionSize;
}

// =====================================================================================
//  任务选择占位页
// =====================================================================================

void UWeaponLobbyWidget::BuildMissionSelectPage(UPanelWidget* StageCanvas)
{
	using namespace WeaponLobbyStyle;

	UVerticalBox* PageColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	StageCanvas->AddChild(PageColumn);

	// 顶部：标题 + 右上返回按钮（本页唯一控件）
	{
		UHorizontalBox* TopRow = WidgetTree->ConstructWidget<UHorizontalBox>();

		{
			UVerticalBox* TitleColumn = WidgetTree->ConstructWidget<UVerticalBox>();
			TitleColumn->AddChildToVerticalBox(MakeText(TEXT("任务终端"), 22, TextPrimary, true, 60));

			UTextBlock* Sub = MakeText(TEXT("SELECT OPERATION"), 9, TextMuted, false, 320);
			UVerticalBoxSlot* SubSlot = TitleColumn->AddChildToVerticalBox(Sub);
			SubSlot->SetPadding(FMargin(0.0f, 3.0f, 0.0f, 0.0f));

			UHorizontalBoxSlot* TitleSlot = TopRow->AddChildToHorizontalBox(TitleColumn);
			TitleSlot->SetVerticalAlignment(VAlign_Center);
			TitleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}

		{
			UButton* BackButton = MakeStyledButton(
				FLinearColor(0.129f, 0.145f, 0.169f, 1.0f),
				Amber,
				FLinearColor(0.780f, 0.525f, 0.098f, 1.0f));
			BackButton->OnClicked.AddDynamic(this, &UWeaponLobbyWidget::HandleMissionBackClicked);

			UHorizontalBox* BackRow = WidgetTree->ConstructWidget<UHorizontalBox>();

			UTextBlock* Arrow = MakeText(TEXT("<"), 12, Amber, true);
			UHorizontalBoxSlot* ArrowSlot = BackRow->AddChildToHorizontalBox(Arrow);
			ArrowSlot->SetVerticalAlignment(VAlign_Center);
			ArrowSlot->SetPadding(FMargin(0.0f, 0.0f, 10.0f, 0.0f));

			UTextBlock* BackLabel = MakeText(TEXT("返回"), 12, TextPrimary, true, 140);
			UHorizontalBoxSlot* LabelSlot = BackRow->AddChildToHorizontalBox(BackLabel);
			LabelSlot->SetVerticalAlignment(VAlign_Center);

			BackButton->AddChild(BackRow);
			if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(BackButton->GetContentSlot()))
			{
				ContentSlot->SetPadding(FMargin(20.0f, 11.0f));
			}

			UHorizontalBoxSlot* ButtonSlot = TopRow->AddChildToHorizontalBox(BackButton);
			ButtonSlot->SetHorizontalAlignment(HAlign_Right);
			ButtonSlot->SetVerticalAlignment(VAlign_Top);
		}

		UVerticalBoxSlot* TopSlot = PageColumn->AddChildToVerticalBox(TopRow);
		TopSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 16.0f));
	}

	// 主体：一个大占位区，等真正的任务列表接入
	{
		UBorder* Placeholder = WidgetTree->ConstructWidget<UBorder>();
		Placeholder->SetBrush(MakeOutlinedBrush(PanelDeepColor, AmberFaint, 1.0f));
		Placeholder->SetPadding(FMargin(0.0f));

		UVerticalBox* Center = WidgetTree->ConstructWidget<UVerticalBox>();
		Placeholder->AddChild(Center);

		{
			USpacer* Gap = WidgetTree->ConstructWidget<USpacer>();
			UVerticalBoxSlot* GapSlot = Center->AddChildToVerticalBox(Gap);
			GapSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}

		{
			UTextBlock* Main = MakeText(TEXT("任务选择区域"), 22, TextMuted, true, 200);
			Main->SetJustification(ETextJustify::Center);
			UVerticalBoxSlot* MainSlot = Center->AddChildToVerticalBox(Main);
			MainSlot->SetHorizontalAlignment(HAlign_Center);
		}

		{
			UTextBlock* Sub = MakeText(TEXT("任务列表与星球地图将在此处接入"), 11, TextMuted, false, 100);
			Sub->SetJustification(ETextJustify::Center);
			UVerticalBoxSlot* SubSlot = Center->AddChildToVerticalBox(Sub);
			SubSlot->SetHorizontalAlignment(HAlign_Center);
			SubSlot->SetPadding(FMargin(0.0f, 8.0f, 0.0f, 0.0f));
		}

		{
			USpacer* Gap = WidgetTree->ConstructWidget<USpacer>();
			UVerticalBoxSlot* GapSlot = Center->AddChildToVerticalBox(Gap);
			GapSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}

		UVerticalBoxSlot* PlaceholderSlot = PageColumn->AddChildToVerticalBox(Placeholder);
		PlaceholderSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		PlaceholderSlot->SetHorizontalAlignment(HAlign_Fill);
		PlaceholderSlot->SetVerticalAlignment(VAlign_Fill);
	}
}

// =====================================================================================
//  状态刷新
// =====================================================================================

void UWeaponLobbyWidget::RefreshLobbyState()
{
	using namespace WeaponLobbyStyle;

	RefreshSessionInfo();

	const UWorld* World = GetWorld();
	const AWeaponGameState* WeaponGameState = World ? World->GetGameState<AWeaponGameState>() : nullptr;

	const int32 MaxPlayers = WeaponGameState ? WeaponGameState->GetMaxPlayers() : RosterSlotCount;
	const int32 ConnectedCount = WeaponGameState ? WeaponGameState->GetConnectedPlayerCount() : 1;

	const APlayerState* LocalPlayerState = GetOwningPlayerState();
	const bool bLocalReady = IsLocalPlayerReady();

	// ---- 小窗 ----
	if (DockCountText)
	{
		DockCountText->SetText(FText::FromString(FString::Printf(TEXT("%d/%d"), ConnectedCount, MaxPlayers)));
	}
	if (DockStatusText)
	{
		DockStatusText->SetText(FText::FromString(bLocalReady ? TEXT("已准备") : TEXT("待命")));
		DockStatusText->SetColorAndOpacity(FSlateColor(bLocalReady ? Ready : Amber));
	}
	if (DockAccentBar)
	{
		DockAccentBar->SetBrushColor(bLocalReady ? Ready : Amber);
	}

	// ---- 顶栏 ----
	if (HeaderCountText)
	{
		HeaderCountText->SetText(
			FText::FromString(FString::Printf(TEXT("小队 %d/%d"), ConnectedCount, MaxPlayers)));
	}
	if (HeaderPhaseText)
	{
		HeaderPhaseText->SetText(FText::FromString(IsLocalHost() ? TEXT("房主 · 待命中") : TEXT("队员 · 待命中")));
	}

	// ---- 名单：按服务端分配的 SlotIndex 排布（槽位 0..MaxPlayers-1 对应行）----
	TArray<APlayerState*> Players;
	if (WeaponGameState)
	{
		for (APlayerState* Candidate : WeaponGameState->PlayerArray)
		{
			if (Candidate && !Candidate->IsOnlyASpectator())
			{
				Players.Add(Candidate);
			}
		}
	}
	else if (APlayerState* Fallback = GetOwningPlayerState())
	{
		Players.Add(Fallback);
	}

	for (int32 RowIndex = 0; RowIndex < RosterRows.Num(); ++RowIndex)
	{
		FWeaponLobbyRosterRow& Row = RosterRows[RowIndex];

		// 超出 MaxPlayers 的行整行隐藏，房间上限变化时布局自动收缩。
		if (Row.RowBackground)
		{
			Row.RowBackground->SetVisibility(
				RowIndex < MaxPlayers ? ESlateVisibility::Visible : ESlateVisibility::Collapsed);
		}
		if (RowIndex >= MaxPlayers)
		{
			continue;
		}

		// 该槽位当前由谁占用：取 SlotIndex == RowIndex 的玩家。
		APlayerState* Player = nullptr;
		for (APlayerState* Candidate : Players)
		{
			const AWeaponPlayerState* WeaponPS = Cast<AWeaponPlayerState>(Candidate);
			if (WeaponPS && WeaponPS->GetPlayerSlotIndex() == RowIndex)
			{
				Player = Candidate;
				break;
			}
		}

		const bool bOccupied = Player != nullptr;
		const bool bIsLocal = bOccupied && Player == LocalPlayerState;

		// 本地玩家与远程玩家统一读复制后的真实 ready 状态。
		const bool bIsReady = bOccupied &&
			(Player == LocalPlayerState ? bLocalReady : Cast<AWeaponPlayerState>(Player)->GetIsPlayerReady());

		if (Row.RowBackground)
		{
			Row.RowBackground->SetBrushColor(
				bOccupied ? (bIsLocal ? PanelHoverColor : PanelRaisedColor) : PanelEmptyColor);
		}
		if (Row.SlotAccent)
		{
			// 本地玩家用实心橙黄标出"你在这一行"。
			Row.SlotAccent->SetBrushColor(
				bIsLocal ? Amber : (bOccupied ? (bIsReady ? Ready : AmberDim) : AmberFaint));
		}
		if (Row.SlotIndexText)
		{
			Row.SlotIndexText->SetColorAndOpacity(FSlateColor(bOccupied ? TextSecondary : TextMuted));
		}
		if (Row.AvatarHintText)
		{
			Row.AvatarHintText->SetText(FText::FromString(bOccupied ? TEXT("矿工") : TEXT("空")));
		}
		if (Row.PlayerNameText)
		{
			// 本次不显示真实玩家名，固定用槽位占位标识；"（你）"标出本地玩家所在行。
			FString DisplayName = FString::Printf(TEXT("矿工 %02d"), RowIndex + 1);
			if (bIsLocal)
			{
				DisplayName += TEXT("（你）");
			}
			Row.PlayerNameText->SetText(FText::FromString(DisplayName));
			Row.PlayerNameText->SetColorAndOpacity(FSlateColor(bOccupied ? TextPrimary : TextMuted));
		}
		if (Row.PlayerMetaText)
		{
			// 玩家信息本次全部占位；接入职业/等级后替换这段字符串即可。
			Row.PlayerMetaText->SetText(FText::FromString(
				bOccupied ? TEXT("职业 --   ·   等级 --   ·   装备 --") : TEXT("等待矿工接入 —")));
			Row.PlayerMetaText->SetColorAndOpacity(FSlateColor(bOccupied ? TextSecondary : TextMuted));
		}
		if (Row.ReadyBadge && Row.ReadyBadgeText)
		{
			if (!bOccupied)
			{
				Row.ReadyBadge->SetBrushColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
				Row.ReadyBadgeText->SetText(FText::FromString(TEXT("—")));
				Row.ReadyBadgeText->SetColorAndOpacity(FSlateColor(TextMuted));
			}
			else if (bIsReady)
			{
				Row.ReadyBadge->SetBrushColor(FLinearColor(0.408f, 0.851f, 0.435f, 0.16f));
				Row.ReadyBadgeText->SetText(FText::FromString(TEXT("已准备")));
				Row.ReadyBadgeText->SetColorAndOpacity(FSlateColor(Ready));
			}
			else
			{
				Row.ReadyBadge->SetBrushColor(FLinearColor(1.0f, 0.686f, 0.145f, 0.12f));
				Row.ReadyBadgeText->SetText(FText::FromString(TEXT("未准备")));
				Row.ReadyBadgeText->SetColorAndOpacity(FSlateColor(Amber));
			}
		}
	}

	// ---- 主操作按钮 ----
	const EWeaponLobbyPrimaryAction Action = ResolvePrimaryAction();

	if (PrimaryActionText && PrimaryActionHintText && PrimaryActionSurface)
	{
		switch (Action)
		{
		case EWeaponLobbyPrimaryAction::Ready:
			PrimaryActionSurface->SetBrushColor(Amber);
			PrimaryActionText->SetText(FText::FromString(TEXT("准备完成")));
			PrimaryActionText->SetColorAndOpacity(FSlateColor(TextOnAmber));
			PrimaryActionHintText->SetText(FText::FromString(TEXT("READY UP")));
			PrimaryActionHintText->SetColorAndOpacity(
				FSlateColor(FLinearColor(0.055f, 0.047f, 0.031f, 0.65f)));
			break;

		case EWeaponLobbyPrimaryAction::Unready:
			// 取消准备是"退一步"的操作，降到低饱和深色，不与开始游戏抢注意力。
			PrimaryActionSurface->SetBrushColor(FLinearColor(0.106f, 0.122f, 0.145f, 1.0f));
			PrimaryActionText->SetText(FText::FromString(TEXT("取消准备")));
			PrimaryActionText->SetColorAndOpacity(FSlateColor(TextSecondary));
			PrimaryActionHintText->SetText(FText::FromString(
				IsLocalHost() ? TEXT("WAITING FOR SQUAD") : TEXT("CANCEL")));
			PrimaryActionHintText->SetColorAndOpacity(FSlateColor(TextMuted));
			break;

		case EWeaponLobbyPrimaryAction::StartGame:
			PrimaryActionSurface->SetBrushColor(Ready);
			PrimaryActionText->SetText(FText::FromString(TEXT("开始游戏")));
			PrimaryActionText->SetColorAndOpacity(FSlateColor(FLinearColor(0.020f, 0.055f, 0.027f, 1.0f)));
			PrimaryActionHintText->SetText(FText::FromString(TEXT("LAUNCH")));
			PrimaryActionHintText->SetColorAndOpacity(
				FSlateColor(FLinearColor(0.020f, 0.055f, 0.027f, 0.65f)));
			break;
		}
	}
}

void UWeaponLobbyWidget::RefreshSessionInfo()
{
	using namespace WeaponLobbyStyle;

	// 模式：中心控制面已认证且本局 MatchId 已建立 → Central Server。
	// Direct/Relay 是客户端候选连通结果，不是新建 Host 的启动前置条件。
	const UWeaponCentralConnectivitySubsystem* Central =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponCentralConnectivitySubsystem>() : nullptr;
	const bool bCentralMode = Central && Central->IsCentralMatchReady();

	if (DockModeText)
	{
		DockModeText->SetText(FText::FromString(bCentralMode ? TEXT("CENTRAL SERVER") : TEXT("LOCAL P2P")));
		DockModeText->SetColorAndOpacity(FSlateColor(bCentralMode ? Amber : TextMuted));
	}

	if (SessionInfoRows.Num() < 3)
	{
		return;
	}

	const bool bIsHost = IsLocalHost();
	const UWorld* World = GetWorld();

	// 模式
	SessionInfoRows[0].Value->SetText(FText::FromString(bCentralMode ? TEXT("Central Server") : TEXT("Local P2P")));
	SessionInfoRows[0].Value->SetColorAndOpacity(FSlateColor(bCentralMode ? Amber : TextPrimary));

	// 角色：房主显示端口，队员显示服务器地址。
	FString RoleText = bIsHost ? TEXT("Host（房主）") : TEXT("Client（队员）");
	if (bIsHost && World)
	{
		RoleText += FString::Printf(TEXT(" · 端口 %d"), World->URL.Port);
	}
	else if (World)
	{
		RoleText += FString::Printf(TEXT(" · %s"), World->URL.Host.IsEmpty() ? TEXT("本机") : *World->URL.Host);
	}
	SessionInfoRows[1].Value->SetText(FText::FromString(RoleText));
	SessionInfoRows[1].Value->SetColorAndOpacity(FSlateColor(bIsHost ? Amber : TextPrimary));

	// 房间号：Central 模式显示 MatchId；Local P2P 显示本地地址。
	FString RoomText;
	if (bCentralMode && Central)
	{
		const FString& MatchId = Central->GetCentralMatchIdString();
		RoomText = MatchId.IsEmpty() ? TEXT("匹配中…") : MatchId;
	}
	else if (World)
	{
		// 房主的 URL.Host 为空，用"本机"占位避免出现 "LAN · :7777"。
		const FString& HostText = World->URL.Host.IsEmpty() ? TEXT("本机") : World->URL.Host;
		RoomText = FString::Printf(TEXT("LAN · %s:%d"), *HostText, World->URL.Port);
	}
	else
	{
		RoomText = TEXT("--");
	}
	SessionInfoRows[2].Value->SetText(FText::FromString(RoomText));
	SessionInfoRows[2].Value->SetColorAndOpacity(FSlateColor(TextSecondary));
}

bool UWeaponLobbyWidget::IsLocalHost() const
{
	// Listen Host 的本地 PlayerController 持有 authority；纯客户端没有。
	const APlayerController* PC = GetOwningPlayer();
	return PC && PC->HasAuthority();
}

bool UWeaponLobbyWidget::AreOtherPlayersReady() const
{
	const UWorld* World = GetWorld();
	const AWeaponGameState* WeaponGameState = World ? World->GetGameState<AWeaponGameState>() : nullptr;
	if (!WeaponGameState)
	{
		return true;
	}

	const APlayerState* LocalPlayerState = GetOwningPlayerState();

	for (APlayerState* Candidate : WeaponGameState->PlayerArray)
	{
		if (!Candidate || Candidate == LocalPlayerState || Candidate->IsOnlyASpectator())
		{
			continue;
		}

		const AWeaponPlayerState* WeaponPS = Cast<AWeaponPlayerState>(Candidate);
		if (!WeaponPS || !WeaponPS->GetIsPlayerReady())
		{
			return false;
		}
	}

	return true;
}

bool UWeaponLobbyWidget::IsLocalPlayerReady() const
{
	const AWeaponPlayerState* PlayerState = Cast<AWeaponPlayerState>(GetOwningPlayerState());
	return PlayerState && PlayerState->GetIsPlayerReady();
}

EWeaponLobbyPrimaryAction UWeaponLobbyWidget::ResolvePrimaryAction() const
{
	if (!IsLocalPlayerReady())
	{
		return EWeaponLobbyPrimaryAction::Ready;
	}

	// 已准备后：房主在全队就绪时升级为"开始游戏"，其余情况都是"取消准备"。
	if (IsLocalHost() && AreOtherPlayersReady())
	{
		return EWeaponLobbyPrimaryAction::StartGame;
	}

	return EWeaponLobbyPrimaryAction::Unready;
}

// =====================================================================================
//  交互
// =====================================================================================

void UWeaponLobbyWidget::HandleExpandClicked()
{
	ExpandLobby();
}

void UWeaponLobbyWidget::HandleCollapseClicked()
{
	CollapseLobby();
}

void UWeaponLobbyWidget::HandleMissionSelectClicked()
{
	SetActivePage(EWeaponLobbyPage::MissionSelect);
}

void UWeaponLobbyWidget::HandleMissionBackClicked()
{
	SetActivePage(EWeaponLobbyPage::Roster);
}

void UWeaponLobbyWidget::HandlePrimaryActionClicked()
{
	switch (ResolvePrimaryAction())
	{
	case EWeaponLobbyPrimaryAction::Ready:
		// 向服务端提交 ready 意图；服务端校验阶段后写入复制属性并广播。
		RequestLocalReady(true);
		break;

	case EWeaponLobbyPrimaryAction::Unready:
		RequestLocalReady(false);
		break;

	case EWeaponLobbyPrimaryAction::StartGame:
		// 只有房主且全员就绪才会走到这里：发起开局倒计时。
		if (AWeaponGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AWeaponGameMode>() : nullptr)
		{
			GameMode->TryStartWeaponMatch();
		}
		break;
	}
}

void UWeaponLobbyWidget::RequestLocalReady(bool bNewIsReady)
{
	AWeaponPlayerState* PlayerState = Cast<AWeaponPlayerState>(GetOwningPlayerState());
	if (PlayerState)
	{
		// 服务端通过 OnLobbyStateChanged 广播回来，RefreshLobbyState 由回调驱动；
		// 本地 Listen Host 的 RequestSetPlayerReady 同步执行 ApplyPlayerReadyRequest，
		// 无需在这里手动刷新。
		PlayerState->RequestSetPlayerReady(bNewIsReady);
	}
}

void UWeaponLobbyWidget::HandleLoadoutButtonClicked()
{
	// 装备 / 天赋 / 职业三个入口本次只做视觉占位，不跳转页面。
}

// =====================================================================================
//  控件工厂
// =====================================================================================

UTextBlock* UWeaponLobbyWidget::MakeText(
	const FString& InText,
	int32 FontSize,
	const FLinearColor& Color,
	bool bBold,
	int32 LetterSpacing)
{
	UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>();
	Text->SetText(FText::FromString(InText));

	FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(bBold ? TEXT("Bold") : TEXT("Regular"), FontSize);
	// 字距是这套 UI 的主要"工业感"来源：小号全大写标签拉开字距，大号标题几乎不拉。
	Font.LetterSpacing = LetterSpacing;
	Text->SetFont(Font);

	Text->SetColorAndOpacity(FSlateColor(Color));
	return Text;
}

UButton* UWeaponLobbyWidget::MakeStyledButton(
	const FLinearColor& NormalColor,
	const FLinearColor& HoveredColor,
	const FLinearColor& PressedColor)
{
	using namespace WeaponLobbyStyle;

	UButton* Button = WidgetTree->ConstructWidget<UButton>();

	// UButton 默认样式是浅灰圆角，在深色面板上非常突兀，必须整体覆盖。
	FButtonStyle Style;
	Style.SetNormal(MakeFlatBrush(NormalColor));
	Style.SetHovered(MakeFlatBrush(HoveredColor));
	Style.SetPressed(MakeFlatBrush(PressedColor));
	Style.SetDisabled(MakeFlatBrush(FLinearColor(0.08f, 0.08f, 0.08f, 0.5f)));
	Style.SetNormalPadding(FMargin(0.0f));
	Style.SetPressedPadding(FMargin(0.0f));
	Button->SetStyle(Style);

	return Button;
}

UBorder* UWeaponLobbyWidget::MakePanel(const FLinearColor& Color, const FMargin& InPadding)
{
	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>();
	Panel->SetBrushColor(Color);
	Panel->SetPadding(InPadding);
	return Panel;
}

UWidget* UWeaponLobbyWidget::MakeHairline(const FLinearColor& Color, float Thickness)
{
	UBorder* Line = WidgetTree->ConstructWidget<UBorder>();
	Line->SetBrushColor(Color);
	Line->SetPadding(FMargin(0.0f));

	USizeBox* LineSize = WidgetTree->ConstructWidget<USizeBox>();
	LineSize->SetHeightOverride(Thickness);
	Line->AddChild(LineSize);

	return Line;
}

UWidget* UWeaponLobbyWidget::MakeSquareActionButton(const FString& Label, const FString& Glyph)
{
	using namespace WeaponLobbyStyle;

	// 正方形按钮：上半是大字符占位，下半是中文说明，三个一行排在名单右上。
	USizeBox* ButtonSize = WidgetTree->ConstructWidget<USizeBox>();
	ButtonSize->SetWidthOverride(84.0f);
	ButtonSize->SetHeightOverride(84.0f);

	UButton* Button = MakeStyledButton(
		PanelRaisedColor,
		FLinearColor(0.176f, 0.196f, 0.227f, 1.0f),
		FLinearColor(0.780f, 0.525f, 0.098f, 1.0f));
	Button->OnClicked.AddDynamic(this, &UWeaponLobbyWidget::HandleLoadoutButtonClicked);
	ButtonSize->AddChild(Button);

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();

	{
		USpacer* Gap = WidgetTree->ConstructWidget<USpacer>();
		UVerticalBoxSlot* GapSlot = Column->AddChildToVerticalBox(Gap);
		GapSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	{
		UTextBlock* GlyphText = MakeText(Glyph, 22, Amber, true);
		GlyphText->SetJustification(ETextJustify::Center);
		UVerticalBoxSlot* GlyphSlot = Column->AddChildToVerticalBox(GlyphText);
		GlyphSlot->SetHorizontalAlignment(HAlign_Center);
	}

	{
		UTextBlock* LabelText = MakeText(Label, 9, TextSecondary, false, 60);
		LabelText->SetJustification(ETextJustify::Center);
		UVerticalBoxSlot* LabelSlot = Column->AddChildToVerticalBox(LabelText);
		LabelSlot->SetHorizontalAlignment(HAlign_Center);
		LabelSlot->SetPadding(FMargin(0.0f, 5.0f, 0.0f, 0.0f));
	}

	{
		USpacer* Gap = WidgetTree->ConstructWidget<USpacer>();
		UVerticalBoxSlot* GapSlot = Column->AddChildToVerticalBox(Gap);
		GapSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	Button->AddChild(Column);
	if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(Button->GetContentSlot()))
	{
		ContentSlot->SetPadding(FMargin(4.0f));
		ContentSlot->SetHorizontalAlignment(HAlign_Fill);
		ContentSlot->SetVerticalAlignment(VAlign_Fill);
	}

	return ButtonSize;
}
