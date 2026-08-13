// Copyright Epic Games, Inc. All Rights Reserved.

#include "Variant_Weapon/UI/WeaponMainMenuWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Brushes/SlateRoundedBoxBrush.h"
#include "Components/Border.h"
#include "Components/Button.h"
#include "Components/ButtonSlot.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SafeZone.h"
#include "Components/ScaleBox.h"
#include "Components/ScrollBox.h"
#include "Components/SizeBox.h"
#include "Components/Spacer.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/WidgetSwitcher.h"
#include "Engine/GameInstance.h"
#include "Kismet/GameplayStatics.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Online/WeaponCentralConnectivitySubsystem.h"
#include "Online/WeaponSessionSubsystem.h"
#include "Styling/CoreStyle.h"
#include "TimerManager.h"

namespace WeaponMainMenuStyle
{
	/** 设计画布尺寸：所有排版数值都以 1920x1080 为基准，外层 ScaleBox 负责等比缩放 */
	static constexpr float DesignWidth = 1920.f;
	static constexpr float DesignHeight = 1080.f;

	/** 玩法关卡路径（单人 / 创建房间都进这张图） */
	static const TCHAR* GameplayLevelPath = TEXT("/Game/Variant_Weapon/LVL_Weapon/NewMap");

	// 暗色科幻矿业终端配色
	static constexpr FLinearColor BackdropDeep(0.028f, 0.036f, 0.045f, 1.0f);   // 最底层背景
	static constexpr FLinearColor BackdropWash(0.055f, 0.075f, 0.090f, 1.0f);   // 左侧竖向亮区，制造光照渐变感
	static constexpr FLinearColor PanelFill(0.052f, 0.065f, 0.078f, 0.97f);     // 右侧深色面板
	static constexpr FLinearColor PanelInner(0.075f, 0.092f, 0.108f, 1.0f);     // 面板内的占位块
	static constexpr FLinearColor PanelEdge(0.16f, 0.20f, 0.23f, 1.0f);         // 面板描边
	static constexpr FLinearColor Amber(1.0f, 0.72f, 0.18f, 1.0f);              // 强调色：琥珀黄
	static constexpr FLinearColor AmberDim(1.0f, 0.72f, 0.18f, 0.28f);          // 强调色低透明度（装饰线）
	static constexpr FLinearColor TextPrimary(0.90f, 0.93f, 0.95f, 1.0f);
	static constexpr FLinearColor TextSecondary(0.54f, 0.61f, 0.67f, 1.0f);
	static constexpr FLinearColor TextMuted(0.36f, 0.42f, 0.47f, 1.0f);
	static constexpr FLinearColor ButtonIdle(0.085f, 0.102f, 0.118f, 1.0f);
	static constexpr FLinearColor ButtonHover(0.130f, 0.155f, 0.175f, 1.0f);
	static constexpr FLinearColor ButtonPress(0.170f, 0.150f, 0.075f, 1.0f);
	static constexpr FLinearColor AmberHover(1.0f, 0.80f, 0.34f, 1.0f);
	static constexpr FLinearColor AmberPress(0.86f, 0.60f, 0.12f, 1.0f);
	static constexpr FLinearColor OnAmber(0.05f, 0.05f, 0.05f, 1.0f);
}

void UWeaponMainMenuWidget::NativeOnInitialized()
{
	using namespace WeaponMainMenuStyle;

	Super::NativeOnInitialized();

	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"));
	}

	// 根：全屏画布
	UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(GetRootWidget());
	if (!RootCanvas)
	{
		RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();
		WidgetTree->RootWidget = RootCanvas;
	}

	// 最底层：铺满屏幕的深色背景，保证画布之外的区域也是暗色
	UBorder* Backdrop = WidgetTree->ConstructWidget<UBorder>();
	Backdrop->SetBrushColor(BackdropDeep);
	UCanvasPanelSlot* BackdropSlot = RootCanvas->AddChildToCanvas(Backdrop);
	BackdropSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
	BackdropSlot->SetOffsets(FMargin(0.f));

	// SafeZone：电视和异形屏的安全区内缩
	USafeZone* SafeArea = WidgetTree->ConstructWidget<USafeZone>();
	UCanvasPanelSlot* SafeSlot = RootCanvas->AddChildToCanvas(SafeArea);
	SafeSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
	SafeSlot->SetOffsets(FMargin(0.f));

	// ScaleBox + SizeBox：把 1920x1080 的设计画布等比缩放到可用空间，构图始终保持 16:9
	UScaleBox* Scaler = WidgetTree->ConstructWidget<UScaleBox>();
	Scaler->SetStretch(EStretch::ScaleToFit);
	SafeArea->AddChild(Scaler);

	USizeBox* DesignBox = WidgetTree->ConstructWidget<USizeBox>();
	DesignBox->SetWidthOverride(DesignWidth);
	DesignBox->SetHeightOverride(DesignHeight);
	Scaler->AddChild(DesignBox);

	UCanvasPanel* DesignCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();
	DesignBox->AddChild(DesignCanvas);

	BuildDesignCanvas(DesignCanvas);

	// 订阅 Session 结果：创建/加入失败时回写状态，搜索完成后刷新房间列表
	if (UWeaponSessionSubsystem* Session = GetSessionSubsystem())
	{
		Session->OnHostComplete.AddDynamic(this, &UWeaponMainMenuWidget::HandleHostSessionComplete);
		Session->OnFindComplete.AddDynamic(this, &UWeaponMainMenuWidget::HandleFindSessionsComplete);
		Session->OnJoinComplete.AddDynamic(this, &UWeaponMainMenuWidget::HandleJoinSessionComplete);
	}

	// 订阅中心连接状态：创建房间等待就绪 / 失败回退弹窗
	if (UWeaponCentralConnectivitySubsystem* Central =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponCentralConnectivitySubsystem>() : nullptr)
	{
		Central->OnConnectivityChanged.AddDynamic(
			this, &UWeaponMainMenuWidget::HandleCentralConnectivityChanged);
	}
}

void UWeaponMainMenuWidget::NativeDestruct()
{
	// Session 子系统的生命周期长于本控件，必须主动解绑
	if (UWeaponSessionSubsystem* Session = GetSessionSubsystem())
	{
		Session->OnHostComplete.RemoveDynamic(this, &UWeaponMainMenuWidget::HandleHostSessionComplete);
		Session->OnFindComplete.RemoveDynamic(this, &UWeaponMainMenuWidget::HandleFindSessionsComplete);
		Session->OnJoinComplete.RemoveDynamic(this, &UWeaponMainMenuWidget::HandleJoinSessionComplete);
	}

	if (UWeaponCentralConnectivitySubsystem* Central =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponCentralConnectivitySubsystem>() : nullptr)
	{
		Central->OnConnectivityChanged.RemoveDynamic(
			this, &UWeaponMainMenuWidget::HandleCentralConnectivityChanged);
	}

	Super::NativeDestruct();
}

void UWeaponMainMenuWidget::BuildDesignCanvas(UCanvasPanel* DesignCanvas)
{
	using namespace WeaponMainMenuStyle;

	// 左侧亮区：一条竖向的浅色带，让左导航区看起来有侧光，右侧自然沉下去
	UBorder* LeftWash = WidgetTree->ConstructWidget<UBorder>();
	LeftWash->SetBrushColor(BackdropWash);
	UCanvasPanelSlot* WashSlot = DesignCanvas->AddChildToCanvas(LeftWash);
	WashSlot->SetAnchors(FAnchors(0.f, 0.f, 0.f, 1.f));
	WashSlot->SetOffsets(FMargin(0.f, 0.f, 830.f, 0.f));

	// 装饰斜线：打破纯矩形分栏的呆板感，压在内容下方
	for (int32 Index = 0; Index < 3; ++Index)
	{
		UBorder* Streak = WidgetTree->ConstructWidget<UBorder>();
		Streak->SetBrushColor(AmberDim);
		UCanvasPanelSlot* StreakSlot = DesignCanvas->AddChildToCanvas(Streak);
		StreakSlot->SetAnchors(FAnchors(0.f, 0.f));
		StreakSlot->SetOffsets(FMargin(-60.f + Index * 150.f, 250.f + Index * 120.f, 420.f, 2.f));
		Streak->SetRenderTransformAngle(-24.f);
		Streak->SetRenderOpacity(0.55f - Index * 0.15f);
	}

	BuildIdentityBar(DesignCanvas);
	BuildBrandBlock(DesignCanvas);
	BuildNavColumn(DesignCanvas);
	BuildRightPanel(DesignCanvas);
}

void UWeaponMainMenuWidget::BuildIdentityBar(UCanvasPanel* DesignCanvas)
{
	using namespace WeaponMainMenuStyle;

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
	UCanvasPanelSlot* RowSlot = DesignCanvas->AddChildToCanvas(Row);
	RowSlot->SetAnchors(FAnchors(0.f, 0.f));
	RowSlot->SetOffsets(FMargin(110.f, 64.f, 660.f, 100.f));

	// 头像占位：方形描边框 + 中文占位文字，等美术资源到位后换成 UImage
	USizeBox* AvatarBox = WidgetTree->ConstructWidget<USizeBox>();
	AvatarBox->SetWidthOverride(92.f);
	AvatarBox->SetHeightOverride(92.f);

	UBorder* AvatarFrame = WidgetTree->ConstructWidget<UBorder>();
	AvatarFrame->SetBrush(FSlateRoundedBoxBrush(PanelInner, 6.f, Amber, 2.f));
	AvatarFrame->SetHorizontalAlignment(HAlign_Center);
	AvatarFrame->SetVerticalAlignment(VAlign_Center);
	AvatarFrame->AddChild(MakeText(TEXT("头像"), TEXT("Regular"), 15.f, TextMuted));
	AvatarBox->AddChild(AvatarFrame);

	UHorizontalBoxSlot* AvatarSlot = Row->AddChildToHorizontalBox(AvatarBox);
	AvatarSlot->SetVerticalAlignment(VAlign_Center);

	// 玩家名 + 档案状态
	UVerticalBox* NameBox = WidgetTree->ConstructWidget<UVerticalBox>();
	NameBox->AddChildToVerticalBox(MakeText(TEXT("指挥官"), TEXT("Bold"), 24.f, TextPrimary));

	UVerticalBoxSlot* SubSlot = NameBox->AddChildToVerticalBox(
		MakeText(TEXT("本地档案 · 未同步"), TEXT("Regular"), 14.f, TextSecondary, 80));
	SubSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));

	UHorizontalBoxSlot* NameSlot = Row->AddChildToHorizontalBox(NameBox);
	NameSlot->SetPadding(FMargin(18.f, 0.f, 0.f, 0.f));
	NameSlot->SetVerticalAlignment(VAlign_Center);

	// 中间留白，把存档按钮推到右端
	UHorizontalBoxSlot* FillSlot = Row->AddChildToHorizontalBox(WidgetTree->ConstructWidget<USpacer>());
	FillSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

	// 管理存档：按需求暂不绑定事件
	UButton* SaveButton = MakeActionButton(TEXT("管理存档"), false);
	UHorizontalBoxSlot* SaveSlot = Row->AddChildToHorizontalBox(SaveButton);
	SaveSlot->SetVerticalAlignment(VAlign_Center);
}

void UWeaponMainMenuWidget::BuildBrandBlock(UCanvasPanel* DesignCanvas)
{
	using namespace WeaponMainMenuStyle;

	UVerticalBox* Block = WidgetTree->ConstructWidget<UVerticalBox>();
	UCanvasPanelSlot* BlockSlot = DesignCanvas->AddChildToCanvas(Block);
	BlockSlot->SetAnchors(FAnchors(0.f, 0.f));
	BlockSlot->SetOffsets(FMargin(110.f, 250.f, 700.f, 300.f));

	// 标题上方的短琥珀色横条，作为视觉起点
	USizeBox* MarkBox = WidgetTree->ConstructWidget<USizeBox>();
	MarkBox->SetWidthOverride(118.f);
	MarkBox->SetHeightOverride(5.f);

	UBorder* Mark = WidgetTree->ConstructWidget<UBorder>();
	Mark->SetBrushColor(Amber);
	MarkBox->AddChild(Mark);

	UVerticalBoxSlot* MarkSlot = Block->AddChildToVerticalBox(MarkBox);
	MarkSlot->SetHorizontalAlignment(HAlign_Left);
	MarkSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 26.f));

	// LDR 主标题：字重拉满，字距放开，作为整个画面的视觉锚点
	Block->AddChildToVerticalBox(MakeText(TEXT("LDR"), TEXT("Black"), 146.f, Amber, 40));

	UVerticalBoxSlot* TagSlot = Block->AddChildToVerticalBox(
		MakeText(TEXT("深岩终端 · 主控台"), TEXT("Regular"), 21.f, TextSecondary, 180));
	TagSlot->SetPadding(FMargin(6.f, 6.f, 0.f, 0.f));
}

void UWeaponMainMenuWidget::BuildNavColumn(UCanvasPanel* DesignCanvas)
{
	using namespace WeaponMainMenuStyle;

	constexpr float ButtonHeight = 62.f;
	constexpr float ButtonGap = 14.f;
	constexpr int32 ButtonCount = 5;
	constexpr float ColumnHeight = ButtonCount * ButtonHeight + (ButtonCount - 1) * ButtonGap;
	constexpr float BottomMargin = 88.f;

	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();
	UCanvasPanelSlot* ColumnSlot = DesignCanvas->AddChildToCanvas(Column);
	// 锚到画布左下角，导航栏始终贴底
	ColumnSlot->SetAnchors(FAnchors(0.f, 1.f));
	ColumnSlot->SetOffsets(FMargin(110.f, -(ColumnHeight + BottomMargin), 352.f, ColumnHeight));

	struct FNavEntry
	{
		const TCHAR* Label;
		FLinearColor Marker;
		FName Handler;
	};

	const FNavEntry Entries[] =
	{
		{ TEXT("单人模式"), Amber,                                    TEXT("HandleSinglePlayerClicked") },
		{ TEXT("多人模式"), Amber,                                    TEXT("HandleMultiplayerClicked") },
		{ TEXT("选项设置"), FLinearColor(0.35f, 0.42f, 0.48f, 1.0f),   TEXT("HandleOptionsClicked") },
		{ TEXT("百科"),     FLinearColor(0.35f, 0.42f, 0.48f, 1.0f),   TEXT("HandleCodexClicked") },
		{ TEXT("退出游戏"), FLinearColor(0.62f, 0.26f, 0.22f, 1.0f),   TEXT("HandleQuitClicked") },
	};

	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Entries); ++Index)
	{
		const FNavEntry& Entry = Entries[Index];

		UButton* NavButton = MakeNavButton(Entry.Label, Entry.Marker);

		FScriptDelegate ClickDelegate;
		ClickDelegate.BindUFunction(this, Entry.Handler);
		NavButton->OnClicked.Add(ClickDelegate);

		USizeBox* HeightBox = WidgetTree->ConstructWidget<USizeBox>();
		HeightBox->SetHeightOverride(ButtonHeight);
		HeightBox->AddChild(NavButton);

		UVerticalBoxSlot* ButtonSlot = Column->AddChildToVerticalBox(HeightBox);
		ButtonSlot->SetPadding(FMargin(0.f, Index == 0 ? 0.f : ButtonGap, 0.f, 0.f));
		ButtonSlot->SetHorizontalAlignment(HAlign_Fill);
	}
}

void UWeaponMainMenuWidget::BuildRightPanel(UCanvasPanel* DesignCanvas)
{
	using namespace WeaponMainMenuStyle;

	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>();
	Panel->SetBrush(FSlateRoundedBoxBrush(PanelFill, 10.f, PanelEdge, 1.5f));
	Panel->SetPadding(FMargin(38.f, 32.f, 38.f, 34.f));

	UCanvasPanelSlot* PanelSlot = DesignCanvas->AddChildToCanvas(Panel);
	PanelSlot->SetAnchors(FAnchors(0.f, 0.f));
	// 右半侧：留出于左栏一致的边距，纵向高于导航区，形成明显的主次分区
	PanelSlot->SetOffsets(FMargin(860.f, 64.f, 940.f, 952.f));

	UVerticalBox* PanelBody = WidgetTree->ConstructWidget<UVerticalBox>();
	Panel->AddChild(PanelBody);

	// 面板头部：标题 + 右上角返回按钮
	UHorizontalBox* Header = WidgetTree->ConstructWidget<UHorizontalBox>();
	UVerticalBoxSlot* HeaderSlot = PanelBody->AddChildToVerticalBox(Header);
	HeaderSlot->SetHorizontalAlignment(HAlign_Fill);

	PanelTitleText = MakeText(TEXT("终端待命"), TEXT("Bold"), 30.f, TextPrimary, 60);
	UHorizontalBoxSlot* TitleSlot = Header->AddChildToHorizontalBox(PanelTitleText);
	TitleSlot->SetVerticalAlignment(VAlign_Center);

	UHorizontalBoxSlot* HeaderFillSlot = Header->AddChildToHorizontalBox(WidgetTree->ConstructWidget<USpacer>());
	HeaderFillSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));

	PanelBackButton = MakeActionButton(TEXT("返回"), false);
	PanelBackButton->OnClicked.AddDynamic(this, &UWeaponMainMenuWidget::HandleBackClicked);
	PanelBackButton->SetVisibility(ESlateVisibility::Hidden);
	UHorizontalBoxSlot* BackSlot = Header->AddChildToHorizontalBox(PanelBackButton);
	BackSlot->SetVerticalAlignment(VAlign_Center);

	// 标题下的琥珀色分隔线
	USizeBox* DividerBox = WidgetTree->ConstructWidget<USizeBox>();
	DividerBox->SetHeightOverride(2.f);

	UBorder* Divider = WidgetTree->ConstructWidget<UBorder>();
	Divider->SetBrushColor(AmberDim);
	DividerBox->AddChild(Divider);

	UVerticalBoxSlot* DividerSlot = PanelBody->AddChildToVerticalBox(DividerBox);
	DividerSlot->SetPadding(FMargin(0.f, 22.f, 0.f, 26.f));

	// 页面切换器占满剩余空间
	PanelSwitcher = WidgetTree->ConstructWidget<UWidgetSwitcher>();
	UVerticalBoxSlot* SwitcherSlot = PanelBody->AddChildToVerticalBox(PanelSwitcher);
	SwitcherSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	SwitcherSlot->SetHorizontalAlignment(HAlign_Fill);
	SwitcherSlot->SetVerticalAlignment(VAlign_Fill);

	BuildIdlePage();                                        // 0
	BuildTeamPage();                                        // 1
	PanelSwitcher->AddChild(BuildComingSoonPage(
		TEXT("画质、音频与操作设置正在制作中。")));           // 2
	PanelSwitcher->AddChild(BuildComingSoonPage(
		TEXT("敌人、矿物与装备资料将在这里展开。")));         // 3

	PanelSwitcher->SetActiveWidgetIndex(IdlePageIndex);
}

void UWeaponMainMenuWidget::BuildIdlePage()
{
	using namespace WeaponMainMenuStyle;

	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();
	PanelSwitcher->AddChild(Page);

	Page->AddChildToVerticalBox(MakeText(TEXT("此区域尚未设计"), TEXT("Bold"), 22.f, TextSecondary));

	UVerticalBoxSlot* HintSlot = Page->AddChildToVerticalBox(
		MakeText(TEXT("从左侧选择一个入口，内容会显示在这里。"), TEXT("Regular"), 16.f, TextMuted));
	HintSlot->SetPadding(FMargin(0.f, 10.f, 0.f, 30.f));

	// 三块占位色块：给未设计的区域一个有节奏的骨架，而不是一片空白
	const float BlockHeights[] = { 96.f, 96.f, 210.f };
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(BlockHeights); ++Index)
	{
		USizeBox* BlockBox = WidgetTree->ConstructWidget<USizeBox>();
		BlockBox->SetHeightOverride(BlockHeights[Index]);

		UBorder* Block = WidgetTree->ConstructWidget<UBorder>();
		Block->SetBrush(FSlateRoundedBoxBrush(PanelInner, 6.f));
		BlockBox->AddChild(Block);

		UVerticalBoxSlot* BlockSlot = Page->AddChildToVerticalBox(BlockBox);
		BlockSlot->SetPadding(FMargin(0.f, Index == 0 ? 0.f : 14.f, 0.f, 0.f));
		BlockSlot->SetHorizontalAlignment(HAlign_Fill);
	}
}

void UWeaponMainMenuWidget::BuildTeamPage()
{
	using namespace WeaponMainMenuStyle;

	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();
	PanelSwitcher->AddChild(Page);

	// 操作行：创建房间（强调）+ 刷新房间
	UHorizontalBox* ActionRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	UVerticalBoxSlot* ActionRowSlot = Page->AddChildToVerticalBox(ActionRow);
	ActionRowSlot->SetHorizontalAlignment(HAlign_Fill);

	UButton* CreateButton = MakeActionButton(TEXT("创建房间"), true);
	CreateButton->OnClicked.AddDynamic(this, &UWeaponMainMenuWidget::HandleCreateRoomClicked);
	CreateRoomButton = CreateButton;
	ActionRow->AddChildToHorizontalBox(CreateButton);

	UButton* RefreshButton = MakeActionButton(TEXT("刷新房间"), false);
	RefreshButton->OnClicked.AddDynamic(this, &UWeaponMainMenuWidget::HandleRefreshRoomsClicked);
	UHorizontalBoxSlot* RefreshSlot = ActionRow->AddChildToHorizontalBox(RefreshButton);
	RefreshSlot->SetPadding(FMargin(14.f, 0.f, 0.f, 0.f));

	UVerticalBoxSlot* CentralLabelSlot = Page->AddChildToVerticalBox(
		MakeText(TEXT("中心服务器房间号"), TEXT("Bold"), 17.f, Amber, 100));
	CentralLabelSlot->SetPadding(FMargin(0.f, 24.f, 0.f, 10.f));

	UHorizontalBox* CentralJoinRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	UVerticalBoxSlot* CentralJoinRowSlot = Page->AddChildToVerticalBox(CentralJoinRow);
	CentralJoinRowSlot->SetHorizontalAlignment(HAlign_Fill);

	CentralJoinCodeInput = WidgetTree->ConstructWidget<UEditableTextBox>();
	FEditableTextBoxStyle JoinCodeStyle =
		FCoreStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>(TEXT("NormalEditableTextBox"));
	JoinCodeStyle.SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 16.f));
	JoinCodeStyle.SetForegroundColor(FSlateColor(TextPrimary));
	JoinCodeStyle.SetBackgroundColor(FSlateColor(PanelInner));
	CentralJoinCodeInput->SetWidgetStyle(JoinCodeStyle);
	CentralJoinCodeInput->SetHintText(FText::FromString(TEXT("输入房主分享的 JoinCode")));
	CentralJoinCodeInput->SetMinDesiredWidth(320.f);
	CentralJoinCodeInput->SetSelectAllTextWhenFocused(true);
	CentralJoinCodeInput->SetClearKeyboardFocusOnCommit(false);
	UHorizontalBoxSlot* JoinCodeInputSlot = CentralJoinRow->AddChildToHorizontalBox(CentralJoinCodeInput);
	JoinCodeInputSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	JoinCodeInputSlot->SetVerticalAlignment(VAlign_Fill);

	CentralJoinButton = MakeActionButton(TEXT("按房间号加入"), false);
	CentralJoinButton->OnClicked.AddDynamic(this, &UWeaponMainMenuWidget::HandleCentralJoinClicked);
	UHorizontalBoxSlot* CentralJoinButtonSlot = CentralJoinRow->AddChildToHorizontalBox(CentralJoinButton);
	CentralJoinButtonSlot->SetPadding(FMargin(14.f, 0.f, 0.f, 0.f));
	CentralJoinButtonSlot->SetVerticalAlignment(VAlign_Fill);

	RoomStatusText = MakeText(TEXT("输入 Central 房间号，或刷新同一局域网内的房间。"), TEXT("Regular"), 16.f, TextSecondary);
	UVerticalBoxSlot* StatusSlot = Page->AddChildToVerticalBox(RoomStatusText);
	StatusSlot->SetPadding(FMargin(0.f, 18.f, 0.f, 0.f));

	UVerticalBoxSlot* LabelSlot = Page->AddChildToVerticalBox(
		MakeText(TEXT("局域网房间"), TEXT("Bold"), 17.f, Amber, 120));
	LabelSlot->SetPadding(FMargin(0.f, 26.f, 0.f, 12.f));

	// 直连地址：房主分享的 frp / IPv4 / hostname:port，配合端口转发、FRP 等工具跨网络联机
	UVerticalBoxSlot* DirectLabelSlot = Page->AddChildToVerticalBox(
		MakeText(TEXT("直连地址 / FRP 联机"), TEXT("Bold"), 17.f, Amber, 120));
	DirectLabelSlot->SetPadding(FMargin(0.f, 26.f, 0.f, 12.f));

	UHorizontalBox* DirectJoinRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	UVerticalBoxSlot* DirectJoinRowSlot = Page->AddChildToVerticalBox(DirectJoinRow);
	DirectJoinRowSlot->SetHorizontalAlignment(HAlign_Fill);

	DirectAddressInput = WidgetTree->ConstructWidget<UEditableTextBox>();
	FEditableTextBoxStyle DirectAddressStyle =
		FCoreStyle::Get().GetWidgetStyle<FEditableTextBoxStyle>(TEXT("NormalEditableTextBox"));
	DirectAddressStyle.SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 16.f));
	DirectAddressStyle.SetForegroundColor(FSlateColor(TextPrimary));
	DirectAddressStyle.SetBackgroundColor(FSlateColor(PanelInner));
	DirectAddressInput->SetWidgetStyle(DirectAddressStyle);
	DirectAddressInput->SetHintText(FText::FromString(TEXT("房主地址，如 1.2.3.4:7777")));
	DirectAddressInput->SetMinDesiredWidth(320.f);
	DirectAddressInput->SetSelectAllTextWhenFocused(true);
	DirectAddressInput->SetClearKeyboardFocusOnCommit(false);
	UHorizontalBoxSlot* DirectAddressInputSlot = DirectJoinRow->AddChildToHorizontalBox(DirectAddressInput);
	DirectAddressInputSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	DirectAddressInputSlot->SetVerticalAlignment(VAlign_Fill);

	DirectJoinButton = MakeActionButton(TEXT("按地址加入"), false);
	DirectJoinButton->OnClicked.AddDynamic(this, &UWeaponMainMenuWidget::HandleDirectJoinClicked);
	UHorizontalBoxSlot* DirectJoinButtonSlot = DirectJoinRow->AddChildToHorizontalBox(DirectJoinButton);
	DirectJoinButtonSlot->SetPadding(FMargin(14.f, 0.f, 0.f, 0.f));
	DirectJoinButtonSlot->SetVerticalAlignment(VAlign_Fill);

	UVerticalBoxSlot* DirectHintSlot = Page->AddChildToVerticalBox(
		MakeText(TEXT("用于 FRP、端口转发等跨网络联机。直接连接不走中心服务器。"), TEXT("Regular"), 14.f, TextMuted));
	DirectHintSlot->SetPadding(FMargin(0.f, 10.f, 0.f, 0.f));

	// 房间列表可能超出面板高度，套一层滚动容器
	UScrollBox* ListScroll = WidgetTree->ConstructWidget<UScrollBox>();
	UVerticalBoxSlot* ScrollSlot = Page->AddChildToVerticalBox(ListScroll);
	ScrollSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	ScrollSlot->SetHorizontalAlignment(HAlign_Fill);
	ScrollSlot->SetVerticalAlignment(VAlign_Fill);

	RoomListBox = WidgetTree->ConstructWidget<UVerticalBox>();
	ListScroll->AddChild(RoomListBox);

	RebuildRoomList(TArray<FWeaponLanSessionResult>());
}

UVerticalBox* UWeaponMainMenuWidget::BuildComingSoonPage(const FString& Hint)
{
	using namespace WeaponMainMenuStyle;

	UVerticalBox* Page = WidgetTree->ConstructWidget<UVerticalBox>();

	Page->AddChildToVerticalBox(MakeText(TEXT("暂未开放"), TEXT("Bold"), 26.f, TextSecondary, 80));

	UVerticalBoxSlot* HintSlot = Page->AddChildToVerticalBox(
		MakeText(Hint, TEXT("Regular"), 16.f, TextMuted));
	HintSlot->SetPadding(FMargin(0.f, 12.f, 0.f, 0.f));

	return Page;
}

UButton* UWeaponMainMenuWidget::MakeNavButton(const FString& Label, const FLinearColor& MarkerColor)
{
	using namespace WeaponMainMenuStyle;

	UButton* Button = WidgetTree->ConstructWidget<UButton>();

	// 三态都用圆角块：静态几乎融进背景，悬停提亮，按下压出琥珀底色
	FButtonStyle Style;
	Style.SetNormal(FSlateRoundedBoxBrush(ButtonIdle, 4.f, FLinearColor(0.14f, 0.17f, 0.20f, 1.0f), 1.f));
	Style.SetHovered(FSlateRoundedBoxBrush(ButtonHover, 4.f, Amber, 1.5f));
	Style.SetPressed(FSlateRoundedBoxBrush(ButtonPress, 4.f, Amber, 1.5f));
	Style.SetDisabled(FSlateRoundedBoxBrush(ButtonIdle, 4.f));
	Style.SetNormalPadding(FMargin(0.f));
	Style.SetPressedPadding(FMargin(0.f));
	Button->SetStyle(Style);

	UHorizontalBox* Content = WidgetTree->ConstructWidget<UHorizontalBox>();

	// 左侧竖向指示条：区分普通项、危险项（退出）
	USizeBox* MarkerBox = WidgetTree->ConstructWidget<USizeBox>();
	MarkerBox->SetWidthOverride(4.f);
	MarkerBox->SetHeightOverride(26.f);

	UBorder* Marker = WidgetTree->ConstructWidget<UBorder>();
	Marker->SetBrushColor(MarkerColor);
	MarkerBox->AddChild(Marker);

	UHorizontalBoxSlot* MarkerSlot = Content->AddChildToHorizontalBox(MarkerBox);
	MarkerSlot->SetVerticalAlignment(VAlign_Center);

	UHorizontalBoxSlot* LabelSlot = Content->AddChildToHorizontalBox(
		MakeText(Label, TEXT("Medium"), 22.f, TextPrimary, 60));
	LabelSlot->SetPadding(FMargin(16.f, 0.f, 0.f, 0.f));
	LabelSlot->SetVerticalAlignment(VAlign_Center);

	if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(Button->AddChild(Content)))
	{
		// 文字左对齐，符合左侧导航的阅读动线
		ContentSlot->SetHorizontalAlignment(HAlign_Left);
		ContentSlot->SetVerticalAlignment(VAlign_Fill);
		ContentSlot->SetPadding(FMargin(18.f, 0.f, 18.f, 0.f));
	}

	return Button;
}

UButton* UWeaponMainMenuWidget::MakeActionButton(const FString& Label, bool bFilled)
{
	using namespace WeaponMainMenuStyle;

	UButton* Button = WidgetTree->ConstructWidget<UButton>();

	FButtonStyle Style;
	if (bFilled)
	{
		// 实心琥珀：面板内的主操作
		Style.SetNormal(FSlateRoundedBoxBrush(Amber, 4.f));
		Style.SetHovered(FSlateRoundedBoxBrush(AmberHover, 4.f));
		Style.SetPressed(FSlateRoundedBoxBrush(AmberPress, 4.f));
	}
	else
	{
		// 描边款：次级操作
		Style.SetNormal(FSlateRoundedBoxBrush(FLinearColor(0.09f, 0.11f, 0.13f, 1.0f), 4.f, FLinearColor(0.24f, 0.29f, 0.33f, 1.0f), 1.f));
		Style.SetHovered(FSlateRoundedBoxBrush(FLinearColor(0.13f, 0.16f, 0.18f, 1.0f), 4.f, Amber, 1.f));
		Style.SetPressed(FSlateRoundedBoxBrush(ButtonPress, 4.f, Amber, 1.f));
	}
	Style.SetDisabled(FSlateRoundedBoxBrush(FLinearColor(0.09f, 0.10f, 0.11f, 1.0f), 4.f));
	Style.SetNormalPadding(FMargin(0.f));
	Style.SetPressedPadding(FMargin(0.f));
	Button->SetStyle(Style);

	UTextBlock* LabelText = MakeText(Label, TEXT("Bold"), 17.f, bFilled ? OnAmber : TextPrimary, 60);

	if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(Button->AddChild(LabelText)))
	{
		ContentSlot->SetHorizontalAlignment(HAlign_Center);
		ContentSlot->SetVerticalAlignment(VAlign_Center);
		ContentSlot->SetPadding(FMargin(26.f, 13.f));
	}

	return Button;
}

UTextBlock* UWeaponMainMenuWidget::MakeText(
	const FString& Text,
	const FName Typeface,
	float FontSize,
	const FLinearColor& Color,
	int32 LetterSpacing)
{
	UTextBlock* Block = WidgetTree->ConstructWidget<UTextBlock>();
	Block->SetText(FText::FromString(Text));

	FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(Typeface, FontSize);
	Font.LetterSpacing = LetterSpacing;
	Block->SetFont(Font);

	Block->SetColorAndOpacity(FSlateColor(Color));
	return Block;
}

void UWeaponMainMenuWidget::ShowPanelPage(int32 PageIndex, const FString& Title)
{
	if (PanelSwitcher)
	{
		PanelSwitcher->SetActiveWidgetIndex(PageIndex);
	}

	if (PanelTitleText)
	{
		PanelTitleText->SetText(FText::FromString(Title));
	}

	if (PanelBackButton)
	{
		// 待命页没有可返回的上一层
		PanelBackButton->SetVisibility(PageIndex == IdlePageIndex
			? ESlateVisibility::Hidden
			: ESlateVisibility::Visible);
	}
}

void UWeaponMainMenuWidget::HandleSinglePlayerClicked()
{
	UGameplayStatics::OpenLevel(this, FName(WeaponMainMenuStyle::GameplayLevelPath));
}

void UWeaponMainMenuWidget::HandleMultiplayerClicked()
{
	ShowPanelPage(TeamPageIndex, TEXT("组队大厅"));
}

void UWeaponMainMenuWidget::HandleOptionsClicked()
{
	ShowPanelPage(OptionsPageIndex, TEXT("选项设置"));
}

void UWeaponMainMenuWidget::HandleCodexClicked()
{
	ShowPanelPage(CodexPageIndex, TEXT("百科"));
}

void UWeaponMainMenuWidget::HandleQuitClicked()
{
	UKismetSystemLibrary::QuitGame(this, GetOwningPlayer(), EQuitPreference::Quit, false);
}

void UWeaponMainMenuWidget::HandleBackClicked()
{
	ShowPanelPage(IdlePageIndex, TEXT("终端待命"));
}

void UWeaponMainMenuWidget::HandleRefreshRoomsClicked()
{
	UWeaponSessionSubsystem* Session = GetSessionSubsystem();
	if (!Session)
	{
		SetRoomStatus(TEXT("Session 子系统不可用。"));
		return;
	}

	if (Session->FindLanGames(50))
	{
		SetRoomStatus(TEXT("正在搜索局域网房间…"));
	}
	else
	{
		SetRoomStatus(TEXT("当前有其他网络操作在进行，稍后再试。"));
	}
}

void UWeaponMainMenuWidget::HandleRoomJoinClicked(int32 ResultIndex)
{
	UWeaponSessionSubsystem* Session = GetSessionSubsystem();
	if (!Session)
	{
		SetRoomStatus(TEXT("Session 子系统不可用。"));
		return;
	}

	if (Session->JoinLanGame(ResultIndex))
	{
		SetRoomStatus(TEXT("正在加入房间…"));
	}
	else
	{
		SetRoomStatus(TEXT("加入请求被拒绝，请重新刷新房间列表。"));
	}
}

void UWeaponMainMenuWidget::HandleFindSessionsComplete(
	bool bWasSuccessful,
	const TArray<FWeaponLanSessionResult>& Results)
{
	if (!bWasSuccessful)
	{
		SetRoomStatus(TEXT("搜索失败，请确认在同一局域网后重试。"));
		RebuildRoomList(TArray<FWeaponLanSessionResult>());
		return;
	}

	SetRoomStatus(FString::Printf(TEXT("搜索完成，找到 %d 个房间。"), Results.Num()));
	RebuildRoomList(Results);
}

void UWeaponMainMenuWidget::HandleHostSessionComplete(bool bWasSuccessful, FString Message)
{
	// 成功时通常已经切换到玩法关卡，只在失败时回显原因
	if (!bWasSuccessful)
	{
		SetRoomStatus(FString::Printf(TEXT("创建失败：%s"), *Message));
	}
}

void UWeaponMainMenuWidget::HandleJoinSessionComplete(bool bWasSuccessful, FString Message)
{
	// 成功时引擎会 ClientTravel 离开菜单，这里只需要把失败原因回显
	if (!bWasSuccessful)
	{
		SetRoomStatus(FString::Printf(TEXT("加入失败：%s"), *Message));
	}
}

UWeaponSessionSubsystem* UWeaponMainMenuWidget::GetSessionSubsystem() const
{
	UGameInstance* GameInstance = GetGameInstance();
	return GameInstance ? GameInstance->GetSubsystem<UWeaponSessionSubsystem>() : nullptr;
}

void UWeaponMainMenuWidget::RebuildRoomList(const TArray<FWeaponLanSessionResult>& Results)
{
	using namespace WeaponMainMenuStyle;

	if (!RoomListBox)
	{
		return;
	}

	RoomListBox->ClearChildren();

	if (Results.Num() == 0)
	{
		RoomListBox->AddChildToVerticalBox(
			MakeText(TEXT("暂无房间。"), TEXT("Regular"), 16.f, TextMuted));
		return;
	}

	for (int32 Index = 0; Index < Results.Num(); ++Index)
	{
		const FWeaponLanSessionResult& Result = Results[Index];

		UBorder* RowFrame = WidgetTree->ConstructWidget<UBorder>();
		RowFrame->SetBrush(FSlateRoundedBoxBrush(PanelInner, 5.f));
		RowFrame->SetPadding(FMargin(18.f, 14.f));

		UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();
		RowFrame->AddChild(Row);

		// 房间名 + 人数、延迟一行副信息
		UVerticalBox* InfoBox = WidgetTree->ConstructWidget<UVerticalBox>();
		InfoBox->AddChildToVerticalBox(
			MakeText(Result.ServerName, TEXT("Bold"), 18.f, TextPrimary));

		UVerticalBoxSlot* MetaSlot = InfoBox->AddChildToVerticalBox(MakeText(
			FString::Printf(TEXT("%d/%d 人 · %d ms"), Result.CurrentPlayers, Result.MaxPlayers, Result.PingInMs),
			TEXT("Regular"), 14.f, TextSecondary));
		MetaSlot->SetPadding(FMargin(0.f, 4.f, 0.f, 0.f));

		UHorizontalBoxSlot* InfoSlot = Row->AddChildToHorizontalBox(InfoBox);
		InfoSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		InfoSlot->SetVerticalAlignment(VAlign_Center);

		// 每行的加入按钮自带 ResultIndex，点击后统一回到 HandleRoomJoinClicked
		UWeaponMainMenuRoomJoinButton* JoinButton = WidgetTree->ConstructWidget<UWeaponMainMenuRoomJoinButton>();

		FButtonStyle JoinStyle;
		JoinStyle.SetNormal(FSlateRoundedBoxBrush(Amber, 4.f));
		JoinStyle.SetHovered(FSlateRoundedBoxBrush(AmberHover, 4.f));
		JoinStyle.SetPressed(FSlateRoundedBoxBrush(AmberPress, 4.f));
		JoinStyle.SetDisabled(FSlateRoundedBoxBrush(FLinearColor(0.09f, 0.10f, 0.11f, 1.0f), 4.f));
		JoinStyle.SetNormalPadding(FMargin(0.f));
		JoinStyle.SetPressedPadding(FMargin(0.f));
		JoinButton->SetStyle(JoinStyle);

		if (UButtonSlot* JoinContentSlot = Cast<UButtonSlot>(
			JoinButton->AddChild(MakeText(TEXT("加入"), TEXT("Bold"), 16.f, OnAmber, 60))))
		{
			JoinContentSlot->SetHorizontalAlignment(HAlign_Center);
			JoinContentSlot->SetVerticalAlignment(VAlign_Center);
			JoinContentSlot->SetPadding(FMargin(22.f, 10.f));
		}

		JoinButton->InitializeRow(Result.ResultIndex);
		JoinButton->OnRoomJoinClicked.AddDynamic(this, &UWeaponMainMenuWidget::HandleRoomJoinClicked);

		UHorizontalBoxSlot* JoinSlot = Row->AddChildToHorizontalBox(JoinButton);
		JoinSlot->SetPadding(FMargin(16.f, 0.f, 0.f, 0.f));
		JoinSlot->SetVerticalAlignment(VAlign_Center);

		UVerticalBoxSlot* RowSlot = RoomListBox->AddChildToVerticalBox(RowFrame);
		RowSlot->SetPadding(FMargin(0.f, Index == 0 ? 0.f : 10.f, 0.f, 0.f));
		RowSlot->SetHorizontalAlignment(HAlign_Fill);
	}
}

void UWeaponMainMenuWidget::SetRoomStatus(const FString& Status)
{
	if (RoomStatusText)
	{
		RoomStatusText->SetText(FText::FromString(Status));
	}
}

// =====================================================================================
//  LocalP2P 回退确认弹窗
// =====================================================================================

namespace
{
	/** 弹窗内的统一文本构造（与主菜单风格一致）。 */
	UTextBlock* MakeDialogText(
		UWidgetTree* Tree,
		const FString& Text,
		const FName& Typeface,
		float FontSize,
		const FLinearColor& Color,
		int32 LetterSpacing = 0)
	{
		UTextBlock* Block = Tree->ConstructWidget<UTextBlock>();
		Block->SetText(FText::FromString(Text));

		FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(Typeface, FontSize);
		Font.LetterSpacing = LetterSpacing;
		Block->SetFont(Font);

		Block->SetColorAndOpacity(FSlateColor(Color));
		return Block;
	}
}

void UWeaponLobbyFallbackDialog::NativeOnInitialized()
{
	using namespace WeaponMainMenuStyle;

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

	// 全屏遮罩：半透明深色，盖住主菜单但透出轮廓
	UBorder* Scrim = WidgetTree->ConstructWidget<UBorder>();
	Scrim->SetBrushColor(FLinearColor(0.02f, 0.03f, 0.04f, 0.78f));
	UCanvasPanelSlot* ScrimSlot = RootCanvas->AddChildToCanvas(Scrim);
	ScrimSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
	ScrimSlot->SetOffsets(FMargin(0.f));

	// 居中弹窗：深色面板 + 琥珀描边 + 标题 / 说明 / 双按钮
	USizeBox* DialogSize = WidgetTree->ConstructWidget<USizeBox>();
	DialogSize->SetWidthOverride(660.f);
	UCanvasPanelSlot* DialogSlot = RootCanvas->AddChildToCanvas(DialogSize);
	DialogSlot->SetAnchors(FAnchors(0.5f, 0.5f, 0.5f, 0.5f));
	DialogSlot->SetAlignment(FVector2D(0.5f, 0.5f));
	// Canvas slot 默认不是 AutoSize，零 offsets 会把 660px 的 SizeBox 压成近零宽度，
	// 导致标题溢出、中文正文逐字换行。让 slot 使用子控件的 desired size。
	DialogSlot->SetAutoSize(true);
	DialogSlot->SetZOrder(1);

	UBorder* DialogFrame = WidgetTree->ConstructWidget<UBorder>();
	DialogFrame->SetBrush(FSlateRoundedBoxBrush(FLinearColor(0.052f, 0.065f, 0.078f, 0.98f), 6.f, Amber, 1.5f));
	DialogFrame->SetPadding(FMargin(34.f, 28.f));
	DialogSize->AddChild(DialogFrame);

	UVerticalBox* DialogColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	DialogFrame->AddChild(DialogColumn);

	UTextBlock* Title = WidgetTree->ConstructWidget<UTextBlock>();
	Title->SetText(FText::FromString(TEXT("中心服务器不可用")));
	Title->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 24.f));
	Title->SetColorAndOpacity(FSlateColor(TextPrimary));
	Title->SetJustification(ETextJustify::Center);
	DialogColumn->AddChildToVerticalBox(Title);

	UTextBlock* Body = WidgetTree->ConstructWidget<UTextBlock>();
	Body->SetText(FText::FromString(
		TEXT("无法连接中心服务器（网络或服务异常）。\n是否以本地点对点（Local P2P）模式创建房间？")));
	Body->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 15.f));
	Body->SetColorAndOpacity(FSlateColor(TextSecondary));
	Body->SetJustification(ETextJustify::Center);
	Body->SetAutoWrapText(true);
	UVerticalBoxSlot* BodySlot = DialogColumn->AddChildToVerticalBox(Body);
	BodySlot->SetPadding(FMargin(0.f, 18.f, 0.f, 28.f));

	UHorizontalBox* ButtonRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	UVerticalBoxSlot* ButtonRowSlot = DialogColumn->AddChildToVerticalBox(ButtonRow);
	ButtonRowSlot->SetHorizontalAlignment(HAlign_Center);

	// 拒绝：次级描边按钮（内容自适应宽度）
	UButton* DeclineButton = WidgetTree->ConstructWidget<UButton>();
	{
		FButtonStyle Style;
		Style.SetNormal(FSlateRoundedBoxBrush(FLinearColor(0.09f, 0.11f, 0.13f, 1.0f), 4.f, FLinearColor(0.24f, 0.29f, 0.33f, 1.0f), 1.f));
		Style.SetHovered(FSlateRoundedBoxBrush(FLinearColor(0.13f, 0.16f, 0.18f, 1.0f), 4.f, Amber, 1.f));
		Style.SetPressed(FSlateRoundedBoxBrush(ButtonPress, 4.f, Amber, 1.f));
		Style.SetNormalPadding(FMargin(0.f));
		Style.SetPressedPadding(FMargin(0.f));
		DeclineButton->SetStyle(Style);
	}
	DeclineButton->OnClicked.AddDynamic(this, &UWeaponLobbyFallbackDialog::HandleDeclineClicked);
	DeclineButton->AddChild(MakeDialogText(WidgetTree, TEXT("返回主菜单"), TEXT("Bold"), 15.f, TextPrimary, 40));
	if (UButtonSlot* DeclineContentSlot = Cast<UButtonSlot>(DeclineButton->GetContentSlot()))
	{
		DeclineContentSlot->SetPadding(FMargin(30.f, 14.f));
	}

	UHorizontalBoxSlot* DeclineSlot = ButtonRow->AddChildToHorizontalBox(DeclineButton);
	DeclineSlot->SetPadding(FMargin(0.f, 0.f, 16.f, 0.f));

	// 接受：实心琥珀主按钮（内容自适应宽度）
	UButton* AcceptButton = WidgetTree->ConstructWidget<UButton>();
	{
		FButtonStyle Style;
		Style.SetNormal(FSlateRoundedBoxBrush(Amber, 4.f));
		Style.SetHovered(FSlateRoundedBoxBrush(AmberHover, 4.f));
		Style.SetPressed(FSlateRoundedBoxBrush(AmberPress, 4.f));
		Style.SetNormalPadding(FMargin(0.f));
		Style.SetPressedPadding(FMargin(0.f));
		AcceptButton->SetStyle(Style);
	}
	AcceptButton->OnClicked.AddDynamic(this, &UWeaponLobbyFallbackDialog::HandleAcceptClicked);
	AcceptButton->AddChild(MakeDialogText(WidgetTree, TEXT("以 Local P2P 继续"), TEXT("Bold"), 15.f, OnAmber, 40));
	if (UButtonSlot* AcceptContentSlot = Cast<UButtonSlot>(AcceptButton->GetContentSlot()))
	{
		AcceptContentSlot->SetPadding(FMargin(30.f, 14.f));
	}

	ButtonRow->AddChildToHorizontalBox(AcceptButton);
}

void UWeaponLobbyFallbackDialog::ShowFallbackDialog(UWidget* InOwningMenu, UWidget* InCreateButton)
{
	OwningMenu = InOwningMenu;
	CreateButton = InCreateButton;
	AddToViewport(200);
}

void UWeaponLobbyFallbackDialog::HandleAcceptClicked()
{
	OnFallbackChosen.Broadcast(true);
	RemoveFromParent();
}

void UWeaponLobbyFallbackDialog::HandleDeclineClicked()
{
	OnFallbackChosen.Broadcast(false);
	RemoveFromParent();
}

// =====================================================================================
//  创建房间：中心优先 + LocalP2P 回退
// =====================================================================================

void UWeaponMainMenuWidget::HandleCreateRoomClicked()
{
	if (RoomCreateState != EWeaponRoomCreateState::Idle)
	{
		return;
	}

	UWeaponCentralConnectivitySubsystem* Central =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponCentralConnectivitySubsystem>() : nullptr;
	if (!Central)
	{
		// 中心子系统不存在（异常环境）时直接走 LocalP2P。
		StartHostingGame();
		return;
	}

	BeginConnectingCentral();
}

void UWeaponMainMenuWidget::HandleCentralJoinClicked()
{
	if (RoomCreateState != EWeaponRoomCreateState::Idle || !CentralJoinCodeInput)
	{
		return;
	}

	const FString JoinCode = CentralJoinCodeInput->GetText().ToString().TrimStartAndEnd();
	if (JoinCode.IsEmpty())
	{
		SetRoomStatus(TEXT("请输入房主分享的 Central 房间号。"));
		return;
	}

	UWeaponCentralConnectivitySubsystem* Central =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponCentralConnectivitySubsystem>() : nullptr;
	if (!Central)
	{
		SetRoomStatus(TEXT("中心连接子系统不可用。"));
		return;
	}

	RoomCreateState = EWeaponRoomCreateState::JoiningCentral;
	SetRoomStatus(TEXT("正在连接中心服务器并校验房间号…"));
	if (CreateRoomButton)
	{
		CreateRoomButton->SetIsEnabled(false);
	}
	if (CentralJoinButton)
	{
		CentralJoinButton->SetIsEnabled(false);
	}

	Central->StartCentralConnect();
	// URL 配置错误等同步失败会通过委托把状态恢复为 Idle。
	if (RoomCreateState != EWeaponRoomCreateState::JoiningCentral)
	{
		return;
	}

	FString JoinError;
	if (!Central->JoinCentralMatch(JoinCode, JoinError))
	{
		RoomCreateState = EWeaponRoomCreateState::Idle;
		if (CreateRoomButton)
		{
			CreateRoomButton->SetIsEnabled(true);
		}
		if (CentralJoinButton)
		{
			CentralJoinButton->SetIsEnabled(true);
		}
		SetRoomStatus(JoinError.IsEmpty() ? TEXT("中心房间加入请求被拒绝。") : JoinError);
	}
}

void UWeaponMainMenuWidget::HandleDirectJoinClicked()
{
	if (!DirectAddressInput)
	{
		return;
	}

	const FString Address = DirectAddressInput->GetText().ToString().TrimStartAndEnd();
	if (Address.IsEmpty())
	{
		SetRoomStatus(TEXT("请输入房主分享的直连地址，如 1.2.3.4:7777。"));
		return;
	}

	UWeaponSessionSubsystem* Session = GetSessionSubsystem();
	if (!Session)
	{
		SetRoomStatus(TEXT("Session 子系统不可用。"));
		return;
	}

	// 成功会 ClientTravel 离开菜单；失败（地址无效 / 有其他网络操作）通过 OnJoinComplete 回显原因。
	if (Session->JoinDirectAddress(Address))
	{
		SetRoomStatus(TEXT("正在连接该地址…"));
	}
}

void UWeaponMainMenuWidget::BeginConnectingCentral()
{
	RoomCreateState = EWeaponRoomCreateState::ConnectingCentral;
	SetRoomStatus(TEXT("正在连接中心服务器…"));

	if (CreateRoomButton)
	{
		CreateRoomButton->SetIsEnabled(false);
	}
	if (CentralJoinButton)
	{
		CentralJoinButton->SetIsEnabled(false);
	}

	UWeaponCentralConnectivitySubsystem* Central =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponCentralConnectivitySubsystem>() : nullptr;
	if (Central)
	{
		Central->StartCentralConnect();
		if (RoomCreateState != EWeaponRoomCreateState::ConnectingCentral)
		{
			return;
		}

		// 发起创建 Match；认证未完成时 CreateCentralMatch 会排队，认证成功后自动补发。
		FString CreateError;
		if (!Central->CreateCentralMatch(CreateError))
		{
			PromptLocalP2PFallback(CreateError.IsEmpty() ? TEXT("无法创建中心 Match") : CreateError);
			return;
		}
	}

	// 等待控制认证和 MatchId；超时未就绪则询问 LocalP2P 回退。
	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().SetTimer(
			CentralWaitTimer,
			this,
			&UWeaponMainMenuWidget::HandleCentralWaitTimeout,
			CentralWaitTimeoutSeconds,
			false);
	}
}

void UWeaponMainMenuWidget::HandleCentralConnectivityChanged(
	EWeaponCentralConnectivityState NewState,
	FString Message)
{
	if (RoomCreateState == EWeaponRoomCreateState::JoiningCentral)
	{
		switch (NewState)
		{
		case EWeaponCentralConnectivityState::Failed:
		case EWeaponCentralConnectivityState::Expired:
			RoomCreateState = EWeaponRoomCreateState::Idle;
			if (CreateRoomButton)
			{
				CreateRoomButton->SetIsEnabled(true);
			}
			if (CentralJoinButton)
			{
				CentralJoinButton->SetIsEnabled(true);
			}
			SetRoomStatus(FString::Printf(TEXT("加入中心房间失败：%s"), *Message));
			break;

		case EWeaponCentralConnectivityState::Connected:
		{
			const UWeaponCentralConnectivitySubsystem* Central =
				GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponCentralConnectivitySubsystem>() : nullptr;
			SetRoomStatus(Central && !Central->GetCentralJoinCode().IsEmpty()
				? TEXT("房间号有效，正在等待房主连接端点…")
				: TEXT("中心认证完成，正在加入房间…"));
			break;
		}

		case EWeaponCentralConnectivityState::DirectConnected:
		case EWeaponCentralConnectivityState::Relayed:
			SetRoomStatus(TEXT("已找到房主，正在进入房间…"));
			break;

		default:
			break;
		}
		return;
	}

	if (RoomCreateState != EWeaponRoomCreateState::ConnectingCentral)
	{
		return;
	}

	switch (NewState)
	{
	case EWeaponCentralConnectivityState::Failed:
	case EWeaponCentralConnectivityState::Expired:
		// 中心明确失败：立即停止等待并询问 LocalP2P。
		PromptLocalP2PFallback(Message);
		break;

	case EWeaponCentralConnectivityState::Connected:
	case EWeaponCentralConnectivityState::DirectConnected:
	case EWeaponCentralConnectivityState::Relayed:
		// 新建 Host 以控制认证 + MatchId 为启动门槛；后续 Lease / Checkpoint
		// 在玩法世界内继续收敛，不阻塞房间创建。
		CheckCentralReadyAndMaybeStart();
		break;

	default:
		// 其他状态（握手进行中）继续等待。
		break;
	}
}

void UWeaponMainMenuWidget::CheckCentralReadyAndMaybeStart()
{
	UWeaponCentralConnectivitySubsystem* Central =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponCentralConnectivitySubsystem>() : nullptr;
	if (!Central)
	{
		PromptLocalP2PFallback(TEXT("中心子系统不可用"));
		return;
	}

	if (Central->IsCentralMatchReady())
	{
		StartHostingGame();
	}
	// 否则继续等待认证或 Create Match 响应。
}

void UWeaponMainMenuWidget::HandleCentralWaitTimeout()
{
	if (RoomCreateState != EWeaponRoomCreateState::ConnectingCentral)
	{
		return;
	}

	// 中心仍在认证/建 Match(未 Failed)时,超时不代表失败:
	// 远程网络下认证管线 3 步 HTTP + Create 可能超过首轮超时。
	// 继续等待而不是立即弹 LocalP2P 回退。
	UWeaponCentralConnectivitySubsystem* Central =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponCentralConnectivitySubsystem>() : nullptr;
	if (Central &&
		(Central->GetConnectivityState() == EWeaponCentralConnectivityState::Authenticating ||
		 (Central->GetConnectivityState() == EWeaponCentralConnectivityState::Connected &&
		  !Central->IsCentralMatchReady())))
	{
		if (UWorld* World = GetWorld())
		{
			World->GetTimerManager().SetTimer(
				CentralWaitTimer,
				this,
				&UWeaponMainMenuWidget::HandleCentralWaitTimeout,
				CentralWaitTimeoutSeconds,
				false);
			return;
		}
	}

	PromptLocalP2PFallback(TEXT("等待中心服务器超时"));
}

void UWeaponMainMenuWidget::PromptLocalP2PFallback(const FString& Reason)
{
	if (RoomCreateState != EWeaponRoomCreateState::ConnectingCentral)
	{
		return;
	}

	RoomCreateState = EWeaponRoomCreateState::FallbackPrompt;
	SetRoomStatus(FString::Printf(TEXT("中心连接失败：%s"), *Reason));

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CentralWaitTimer);
	}

	if (FallbackDialog)
	{
		return;
	}

	FallbackDialog = CreateWidget<UWeaponLobbyFallbackDialog>(this, UWeaponLobbyFallbackDialog::StaticClass());
	if (FallbackDialog)
	{
		FallbackDialog->ShowFallbackDialog(this, CreateRoomButton);
		FallbackDialog->OnFallbackChosen.AddDynamic(this, &UWeaponMainMenuWidget::HandleFallbackChosen);
	}
}

void UWeaponMainMenuWidget::HandleFallbackChosen(bool bAccepted)
{
	if (FallbackDialog)
	{
		FallbackDialog->OnFallbackChosen.RemoveDynamic(this, &UWeaponMainMenuWidget::HandleFallbackChosen);
		FallbackDialog = nullptr;
	}

	if (bAccepted)
	{
		StartHostingGame();
	}
	else
	{
		// 拒绝：留在主菜单，恢复创建按钮与状态。
		RoomCreateState = EWeaponRoomCreateState::Idle;
		if (CreateRoomButton)
		{
			CreateRoomButton->SetIsEnabled(true);
		}
		if (CentralJoinButton)
		{
			CentralJoinButton->SetIsEnabled(true);
		}
		SetRoomStatus(TEXT("已取消创建房间。"));
	}
}

void UWeaponMainMenuWidget::StartHostingGame()
{
	RoomCreateState = EWeaponRoomCreateState::Idle;

	if (UWorld* World = GetWorld())
	{
		World->GetTimerManager().ClearTimer(CentralWaitTimer);
	}
	if (CreateRoomButton)
	{
		CreateRoomButton->SetIsEnabled(true);
	}
	if (CentralJoinButton)
	{
		CentralJoinButton->SetIsEnabled(true);
	}

	UWeaponSessionSubsystem* Session = GetSessionSubsystem();
	if (!Session)
	{
		SetRoomStatus(TEXT("Session 子系统不可用。"));
		return;
	}

	SetRoomStatus(TEXT("正在创建房间…"));
	if (!Session->HostLanGameOnMap(4, TEXT("LDR 房间"), FName(WeaponMainMenuStyle::GameplayLevelPath)))
	{
		SetRoomStatus(TEXT("创建请求被拒绝，稍后再试。"));
	}
}
