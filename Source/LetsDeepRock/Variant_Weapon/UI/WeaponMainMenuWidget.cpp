// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponMainMenuWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
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
#include "Engine/GameInstance.h"
#include "Kismet/GameplayStatics.h"
#include "Online/WeaponSessionSubsystem.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"

namespace WeaponMainMenuStyle
{
	/** 背景：接近黑的冷调金属 */
	static const FLinearColor BackdropColor(0.016f, 0.020f, 0.026f, 1.0f);

	/** 按钮常态 */
	static const FLinearColor ButtonColor(0.055f, 0.066f, 0.078f, 1.0f);

	/** 按钮悬停：抬起一档 */
	static const FLinearColor ButtonHoverColor(0.106f, 0.125f, 0.149f, 1.0f);

	/** 按钮按下：橙黄压色 */
	static const FLinearColor ButtonPressedColor(0.780f, 0.525f, 0.098f, 1.0f);

	/** 主强调色：安全帽橙黄 */
	static const FLinearColor Amber(1.000f, 0.686f, 0.145f, 1.0f);

	static const FLinearColor AmberDim(1.000f, 0.686f, 0.145f, 0.32f);

	static const FLinearColor AmberFaint(1.000f, 0.686f, 0.145f, 0.14f);

	static const FLinearColor TextPrimary(0.925f, 0.941f, 0.957f, 1.0f);

	static const FLinearColor TextSecondary(0.573f, 0.616f, 0.667f, 1.0f);

	static const FLinearColor TextMuted(0.353f, 0.388f, 0.435f, 1.0f);

	/** 玩法关卡：单人 OpenLevel 与多人 HostLanGame 都指向它 */
	static const TCHAR* GameplayLevelPath = TEXT("/Game/Variant_Weapon/LVL_Weapon/NewMap");

	/** HostLanGame 需要一个 FName 形式的目标地图，使用完整包路径避免从主菜单解析短名称失败。 */
	static const TCHAR* GameplayLevelName = TEXT("/Game/Variant_Weapon/LVL_Weapon/NewMap");

	/** 默认房间名 */
	static const TCHAR* DefaultServerName = TEXT("Deep Rock LAN");

	/** 四人合作上限 */
	static constexpr int32 DefaultMaxPlayers = 4;

	static constexpr float DesignWidth = 1280.0f;
	static constexpr float DesignHeight = 760.0f;

	/** 纯色 Brush；用 RoundedBox 保证无贴图时也能出色。 */
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
}

void UWeaponMainMenuWidget::NativeOnInitialized()
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

	BuildLayout(RootCanvas);
}

void UWeaponMainMenuWidget::BuildLayout(UPanelWidget* RootCanvas)
{
	using namespace WeaponMainMenuStyle;

	UOverlay* ShellOverlay = WidgetTree->ConstructWidget<UOverlay>();
	{
		UCanvasPanelSlot* ShellSlot = Cast<UCanvasPanelSlot>(RootCanvas->AddChild(ShellOverlay));
		ShellSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
		ShellSlot->SetOffsets(FMargin(0.0f));
	}

	// 底层：全屏纯色背景
	{
		UBorder* Backdrop = WidgetTree->ConstructWidget<UBorder>();
		Backdrop->SetBrushColor(BackdropColor);
		UOverlaySlot* BackdropSlot = ShellOverlay->AddChildToOverlay(Backdrop);
		BackdropSlot->SetHorizontalAlignment(HAlign_Fill);
		BackdropSlot->SetVerticalAlignment(VAlign_Fill);
	}

	// 内容层：SafeZone -> ScaleBox -> 设计尺寸
	USafeZone* SafeArea = WidgetTree->ConstructWidget<USafeZone>();
	{
		UOverlaySlot* SafeSlot = ShellOverlay->AddChildToOverlay(SafeArea);
		SafeSlot->SetHorizontalAlignment(HAlign_Fill);
		SafeSlot->SetVerticalAlignment(VAlign_Fill);
		SafeSlot->SetPadding(FMargin(40.0f, 32.0f));
	}

	UScaleBox* Scaler = WidgetTree->ConstructWidget<UScaleBox>();
	Scaler->SetStretch(EStretch::ScaleToFit);
	SafeArea->AddChild(Scaler);

	USizeBox* DesignBox = WidgetTree->ConstructWidget<USizeBox>();
	DesignBox->SetWidthOverride(DesignWidth);
	DesignBox->SetHeightOverride(DesignHeight);
	Scaler->AddChild(DesignBox);

	// 整体左对齐的单列：标题在上，按钮在中下，形成明确的阅读起点
	UVerticalBox* Column = WidgetTree->ConstructWidget<UVerticalBox>();
	DesignBox->AddChild(Column);

	// ---- 顶部留白 ----
	{
		USpacer* TopGap = WidgetTree->ConstructWidget<USpacer>();
		UVerticalBoxSlot* GapSlot = Column->AddChildToVerticalBox(TopGap);
		GapSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}

	// ---- 标题组：橙黄竖条 + 主标题 + 英文副标 ----
	{
		UHorizontalBox* TitleRow = WidgetTree->ConstructWidget<UHorizontalBox>();

		{
			UBorder* TitleAccent = WidgetTree->ConstructWidget<UBorder>();
			TitleAccent->SetBrushColor(Amber);

			USizeBox* AccentSize = WidgetTree->ConstructWidget<USizeBox>();
			AccentSize->SetWidthOverride(6.0f);
			AccentSize->SetHeightOverride(96.0f);
			TitleAccent->AddChild(AccentSize);

			UHorizontalBoxSlot* AccentSlot = TitleRow->AddChildToHorizontalBox(TitleAccent);
			AccentSlot->SetVerticalAlignment(VAlign_Center);
			AccentSlot->SetPadding(FMargin(0.0f, 0.0f, 22.0f, 0.0f));
		}

		{
			UVerticalBox* TitleColumn = WidgetTree->ConstructWidget<UVerticalBox>();

			UTextBlock* Sub = MakeText(TEXT("DEEP ROCK · MINING OPERATIONS"), 11, TextMuted, false, 380);
			UVerticalBoxSlot* SubSlot = TitleColumn->AddChildToVerticalBox(Sub);
			SubSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 8.0f));

			TitleColumn->AddChildToVerticalBox(MakeText(TEXT("深岩勘探"), 54, TextPrimary, true, 40));

			UTextBlock* Tagline = MakeText(TEXT("ROCK AND STONE"), 12, Amber, true, 420);
			UVerticalBoxSlot* TaglineSlot = TitleColumn->AddChildToVerticalBox(Tagline);
			TaglineSlot->SetPadding(FMargin(0.0f, 10.0f, 0.0f, 0.0f));

			UHorizontalBoxSlot* TitleColumnSlot = TitleRow->AddChildToHorizontalBox(TitleColumn);
			TitleColumnSlot->SetVerticalAlignment(VAlign_Center);
		}

		UVerticalBoxSlot* TitleRowSlot = Column->AddChildToVerticalBox(TitleRow);
		TitleRowSlot->SetHorizontalAlignment(HAlign_Left);
		TitleRowSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 56.0f));
	}

	// ---- 按钮组 ----
	{
		UVerticalBox* ButtonColumn = WidgetTree->ConstructWidget<UVerticalBox>();

		BuildMenuButton(ButtonColumn, TEXT("单人游戏"), TEXT("SOLO DEPLOY"), true);
		BuildMenuButton(ButtonColumn, TEXT("多人游戏"), TEXT("HOST LAN SQUAD"), false);

		UVerticalBoxSlot* ButtonColumnSlot = Column->AddChildToVerticalBox(ButtonColumn);
		ButtonColumnSlot->SetHorizontalAlignment(HAlign_Left);
	}

	// ---- 状态提示行 ----
	{
		StatusText = MakeText(TEXT(""), 11, TextSecondary, false, 120);
		UVerticalBoxSlot* StatusSlot = Column->AddChildToVerticalBox(StatusText);
		StatusSlot->SetHorizontalAlignment(HAlign_Left);
		StatusSlot->SetPadding(FMargin(4.0f, 18.0f, 0.0f, 0.0f));
	}

	// ---- 底部留白 ----
	{
		USpacer* BottomGap = WidgetTree->ConstructWidget<USpacer>();
		UVerticalBoxSlot* GapSlot = Column->AddChildToVerticalBox(BottomGap);
		GapSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	}
}

UButton* UWeaponMainMenuWidget::BuildMenuButton(
	UVerticalBox* Parent,
	const FString& Title,
	const FString& Subtitle,
	bool bIsPrimary)
{
	using namespace WeaponMainMenuStyle;

	// 固定宽度让两个按钮左边缘和右边缘都对齐，形成稳定的竖向节奏。
	USizeBox* ButtonSize = WidgetTree->ConstructWidget<USizeBox>();
	ButtonSize->SetWidthOverride(420.0f);

	UButton* Button = WidgetTree->ConstructWidget<UButton>();
	{
		FButtonStyle Style;
		Style.SetNormal(MakeFlatBrush(ButtonColor));
		Style.SetHovered(MakeFlatBrush(ButtonHoverColor));
		Style.SetPressed(MakeFlatBrush(ButtonPressedColor));
		Style.SetDisabled(MakeFlatBrush(FLinearColor(0.08f, 0.08f, 0.08f, 0.5f)));
		Style.SetNormalPadding(FMargin(0.0f));
		Style.SetPressedPadding(FMargin(0.0f));
		Button->SetStyle(Style);
	}
	ButtonSize->AddChild(Button);

	if (bIsPrimary)
	{
		Button->OnClicked.AddDynamic(this, &UWeaponMainMenuWidget::HandleSinglePlayerClicked);
	}
	else
	{
		Button->OnClicked.AddDynamic(this, &UWeaponMainMenuWidget::HandleMultiplayerClicked);
	}

	// 按钮内部：左侧一根竖条（主按钮实心橙黄，次按钮低亮）+ 文本组
	UHorizontalBox* ButtonRow = WidgetTree->ConstructWidget<UHorizontalBox>();

	{
		UBorder* EdgeAccent = WidgetTree->ConstructWidget<UBorder>();
		EdgeAccent->SetBrushColor(bIsPrimary ? Amber : AmberDim);

		USizeBox* EdgeSize = WidgetTree->ConstructWidget<USizeBox>();
		EdgeSize->SetWidthOverride(4.0f);
		EdgeAccent->AddChild(EdgeSize);

		UHorizontalBoxSlot* EdgeSlot = ButtonRow->AddChildToHorizontalBox(EdgeAccent);
		EdgeSlot->SetVerticalAlignment(VAlign_Fill);
	}

	{
		UVerticalBox* TextColumn = WidgetTree->ConstructWidget<UVerticalBox>();

		TextColumn->AddChildToVerticalBox(
			MakeText(Title, 22, bIsPrimary ? TextPrimary : TextSecondary, true, 100));

		UTextBlock* SubLabel = MakeText(Subtitle, 9, TextMuted, false, 320);
		UVerticalBoxSlot* SubSlot = TextColumn->AddChildToVerticalBox(SubLabel);
		SubSlot->SetPadding(FMargin(0.0f, 5.0f, 0.0f, 0.0f));

		UHorizontalBoxSlot* TextSlot = ButtonRow->AddChildToHorizontalBox(TextColumn);
		TextSlot->SetVerticalAlignment(VAlign_Center);
		TextSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		TextSlot->SetPadding(FMargin(22.0f, 18.0f, 18.0f, 18.0f));
	}

	Button->AddChild(ButtonRow);
	if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(Button->GetContentSlot()))
	{
		ContentSlot->SetPadding(FMargin(0.0f));
		ContentSlot->SetHorizontalAlignment(HAlign_Fill);
		ContentSlot->SetVerticalAlignment(VAlign_Fill);
	}

	UVerticalBoxSlot* ButtonSlot = Parent->AddChildToVerticalBox(ButtonSize);
	ButtonSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 12.0f));
	ButtonSlot->SetHorizontalAlignment(HAlign_Left);

	return Button;
}

void UWeaponMainMenuWidget::HandleSinglePlayerClicked()
{
	using namespace WeaponMainMenuStyle;

	if (bTransitionStarted)
	{
		return;
	}
	bTransitionStarted = true;

	if (StatusText)
	{
		StatusText->SetText(FText::FromString(TEXT("正在进入作业区…")));
	}

	// 单人不建 Session，直接加载玩法关卡。
	UGameplayStatics::OpenLevel(this, FName(GameplayLevelPath));
}

void UWeaponMainMenuWidget::HandleMultiplayerClicked()
{
	using namespace WeaponMainMenuStyle;

	if (bTransitionStarted)
	{
		return;
	}

	UGameInstance* GameInstance = GetGameInstance();
	UWeaponSessionSubsystem* SessionSubsystem =
		GameInstance ? GameInstance->GetSubsystem<UWeaponSessionSubsystem>() : nullptr;

	if (!SessionSubsystem)
	{
		if (StatusText)
		{
			StatusText->SetText(FText::FromString(TEXT("无法创建房间：Session 子系统不可用")));
			StatusText->SetColorAndOpacity(FSlateColor(Amber));
		}
		return;
	}

	if (StatusText)
	{
		StatusText->SetText(FText::FromString(TEXT("正在创建局域网房间…")));
		StatusText->SetColorAndOpacity(FSlateColor(TextSecondary));
	}

	// HostLanGame 内部会以 ?listen 打开目标地图并广播 LAN Session。
	const bool bHostStarted = SessionSubsystem->HostLanGame(
		DefaultMaxPlayers,
		FString(DefaultServerName),
		FName(GameplayLevelName));

	if (bHostStarted)
	{
		bTransitionStarted = true;
	}
	else if (StatusText)
	{
		// 失败时保持按钮可用，允许玩家重试。
		StatusText->SetText(FText::FromString(TEXT("创建房间失败，请重试")));
		StatusText->SetColorAndOpacity(FSlateColor(Amber));
	}
}

UTextBlock* UWeaponMainMenuWidget::MakeText(
	const FString& InText,
	int32 FontSize,
	const FLinearColor& Color,
	bool bBold,
	int32 LetterSpacing)
{
	UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>();
	Text->SetText(FText::FromString(InText));

	FSlateFontInfo Font = FCoreStyle::GetDefaultFontStyle(bBold ? TEXT("Bold") : TEXT("Regular"), FontSize);
	Font.LetterSpacing = LetterSpacing;
	Text->SetFont(Font);

	Text->SetColorAndOpacity(FSlateColor(Color));
	return Text;
}
