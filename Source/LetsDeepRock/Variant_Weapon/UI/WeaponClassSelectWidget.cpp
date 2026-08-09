// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponClassSelectWidget.h"

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
#include "Data/GameDataSubsystem.h"
#include "Engine/GameInstance.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"
#include "WeaponPlayerController.h"

namespace WeaponClassSelectStyle
{
	/** 全屏遮罩：接近黑的冷调 */
	static const FLinearColor ScrimColor(0.05f, 0.1f, 0.12f, 0.92f);

	/** 面板：舱室金属板 */
	static const FLinearColor PanelColor(0.043f, 0.052f, 0.063f, 0.96f);

	/** 凹槽 */
	static const FLinearColor PanelDeepColor(0.024f, 0.030f, 0.038f, 0.98f);

	/** 卡片默认 */
	static const FLinearColor CardDefault(0.15f, 0.2f, 0.25f, 0.95f);

	/** 卡片选中：橙黄描边 + 抬起底色 */
	static const FLinearColor CardSelected(0.070f, 0.082f, 0.098f, 0.96f);

	/** 主强调色：安全帽橙黄 */
	static const FLinearColor Amber(1.000f, 0.686f, 0.145f, 1.0f);

	static const FLinearColor AmberDim(1.000f, 0.686f, 0.145f, 0.32f);

	static const FLinearColor AmberFaint(1.000f, 0.686f, 0.145f, 0.14f);

	static const FLinearColor TextPrimary(0.925f, 0.941f, 0.957f, 1.0f);

	static const FLinearColor TextSecondary(0.573f, 0.616f, 0.667f, 1.0f);

	static const FLinearColor TextMuted(0.353f, 0.388f, 0.435f, 1.0f);

	static constexpr float DesignWidth = 1280.0f;
	static constexpr float DesignHeight = 800.0f;

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

void UWeaponClassSelectWidget::NativeOnInitialized()
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

void UWeaponClassSelectWidget::BuildLayout(UPanelWidget* RootCanvas)
{
	using namespace WeaponClassSelectStyle;

	UOverlay* ShellOverlay = WidgetTree->ConstructWidget<UOverlay>();
	{
		UCanvasPanelSlot* ShellSlot = Cast<UCanvasPanelSlot>(RootCanvas->AddChild(ShellOverlay));
		ShellSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
		ShellSlot->SetOffsets(FMargin(0.0f));
	}

	// 第 0 层：全屏半透明遮罩
	{
		UBorder* Scrim = WidgetTree->ConstructWidget<UBorder>();
		Scrim->SetBrushColor(ScrimColor);
		UOverlaySlot* ScrimSlot = ShellOverlay->AddChildToOverlay(Scrim);
		ScrimSlot->SetHorizontalAlignment(HAlign_Fill);
		ScrimSlot->SetVerticalAlignment(VAlign_Fill);
	}

	// 第 1 层：SafeZone -> ScaleBox -> 设计尺寸 -> 单页内容
	USafeZone* SafeArea = WidgetTree->ConstructWidget<USafeZone>();
	{
		UOverlaySlot* SafeSlot = ShellOverlay->AddChildToOverlay(SafeArea);
		SafeSlot->SetHorizontalAlignment(HAlign_Fill);
		SafeSlot->SetVerticalAlignment(VAlign_Fill);
		SafeSlot->SetPadding(FMargin(20.0f, 20.0f));
	}

	UScaleBox* Scaler = WidgetTree->ConstructWidget<UScaleBox>();
	Scaler->SetStretch(EStretch::ScaleToFit);
	SafeArea->AddChild(Scaler);

	USizeBox* DesignBox = WidgetTree->ConstructWidget<USizeBox>();
	DesignBox->SetWidthOverride(DesignWidth);
	DesignBox->SetHeightOverride(DesignHeight);
	Scaler->AddChild(DesignBox);

	// 单页垂直三段：模型展示（Fill）、职业按钮行（Automatic）、开始游戏（Automatic）
	UVerticalBox* PageColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	DesignBox->AddChild(PageColumn);

	// ---- 上段：大型模型展示占位，Fill 吃掉全部剩余高度 ----
	{
		UVerticalBoxSlot* PreviewSlot = PageColumn->AddChildToVerticalBox(BuildModelPreviewFrame());
		PreviewSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		PreviewSlot->SetHorizontalAlignment(HAlign_Fill);
		PreviewSlot->SetVerticalAlignment(VAlign_Fill);
		PreviewSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 32.0f));
	}

	// ---- 中段：横排职业按钮 ----
	{
		UVerticalBoxSlot* ButtonRowSlot = PageColumn->AddChildToVerticalBox(BuildProfessionButtonRow());
		ButtonRowSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
		ButtonRowSlot->SetHorizontalAlignment(HAlign_Center);
		ButtonRowSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 28.0f));
	}

	// ---- 下段：开始游戏 ----
	{
		UVerticalBoxSlot* StartSlot = PageColumn->AddChildToVerticalBox(BuildStartGameButton());
		StartSlot->SetSize(FSlateChildSize(ESlateSizeRule::Automatic));
		StartSlot->SetHorizontalAlignment(HAlign_Center);
	}
}

UWidget* UWeaponClassSelectWidget::BuildModelPreviewFrame()
{
	using namespace WeaponClassSelectStyle;

	UBorder* Frame = WidgetTree->ConstructWidget<UBorder>();
	Frame->SetBrush(MakeOutlinedBrush(PanelDeepColor, AmberFaint, 1.0f));
	Frame->SetPadding(FMargin(0.0f));

	UVerticalBox* FrameColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	Frame->AddChild(FrameColumn);

	// 顶部标签条
	{
		UBorder* TagBar = WidgetTree->ConstructWidget<UBorder>();
		TagBar->SetBrushColor(FLinearColor(0.086f, 0.098f, 0.114f, 1.0f));
		TagBar->SetPadding(FMargin(20.0f, 11.0f));

		UHorizontalBox* TagRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		TagBar->AddChild(TagRow);

		{
			UTextBlock* Tag = MakeText(TEXT("角色模型"), 11, Amber, true, 200);
			UHorizontalBoxSlot* TagSlot = TagRow->AddChildToHorizontalBox(Tag);
			TagSlot->SetVerticalAlignment(VAlign_Center);
			TagSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}

		{
			UTextBlock* Code = MakeText(TEXT("PREVIEW"), 9, TextMuted, false, 280);
			UHorizontalBoxSlot* CodeSlot = TagRow->AddChildToHorizontalBox(Code);
			CodeSlot->SetVerticalAlignment(VAlign_Center);
		}

		FrameColumn->AddChildToVerticalBox(TagBar);
	}

	// 中部：模型预览占位
	{
		UOverlay* Stage = WidgetTree->ConstructWidget<UOverlay>();

		{
			UBorder* StageBack = WidgetTree->ConstructWidget<UBorder>();
			StageBack->SetBrushColor(FLinearColor(0.016f, 0.020f, 0.026f, 1.0f));
			UOverlaySlot* BackSlot = Stage->AddChildToOverlay(StageBack);
			BackSlot->SetHorizontalAlignment(HAlign_Fill);
			BackSlot->SetVerticalAlignment(VAlign_Fill);
		}

		{
			UVerticalBox* Hint = WidgetTree->ConstructWidget<UVerticalBox>();

			{
				UBorder* Glyph = WidgetTree->ConstructWidget<UBorder>();
				Glyph->SetBrush(MakeOutlinedBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.02f), AmberDim, 1.0f));

				USizeBox* GlyphSize = WidgetTree->ConstructWidget<USizeBox>();
				GlyphSize->SetWidthOverride(180.0f);
				GlyphSize->SetHeightOverride(280.0f);
				Glyph->AddChild(GlyphSize);

				UVerticalBoxSlot* GlyphSlot = Hint->AddChildToVerticalBox(Glyph);
				GlyphSlot->SetHorizontalAlignment(HAlign_Center);
				GlyphSlot->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 20.0f));
			}

			{
				UTextBlock* HintText = MakeText(TEXT("CHARACTER MODEL"), 11, TextMuted, true, 340);
				HintText->SetJustification(ETextJustify::Center);
				UVerticalBoxSlot* HintSlot = Hint->AddChildToVerticalBox(HintText);
				HintSlot->SetHorizontalAlignment(HAlign_Center);
			}

			{
				UTextBlock* SubText = MakeText(TEXT("角色模型占位"), 10, TextMuted, false, 140);
				SubText->SetJustification(ETextJustify::Center);
				UVerticalBoxSlot* SubSlot = Hint->AddChildToVerticalBox(SubText);
				SubSlot->SetHorizontalAlignment(HAlign_Center);
				SubSlot->SetPadding(FMargin(0.0f, 5.0f, 0.0f, 0.0f));
			}

			UOverlaySlot* HintOverlaySlot = Stage->AddChildToOverlay(Hint);
			HintOverlaySlot->SetHorizontalAlignment(HAlign_Center);
			HintOverlaySlot->SetVerticalAlignment(VAlign_Center);
		}

		UVerticalBoxSlot* StageSlot = FrameColumn->AddChildToVerticalBox(Stage);
		StageSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		StageSlot->SetHorizontalAlignment(HAlign_Fill);
		StageSlot->SetVerticalAlignment(VAlign_Fill);
	}

	return Frame;
}

UWidget* UWeaponClassSelectWidget::BuildProfessionButtonRow()
{
	using namespace WeaponClassSelectStyle;

	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

	// 三职业硬编码：快速原型优先最短稳定实现
	struct FProfessionSpec
	{
		FName Id;
		const TCHAR* Name;
		const TCHAR* Subtitle;
	};

	const FProfessionSpec Specs[] =
	{
		{ TEXT("Ranger"),        TEXT("游侠"),   TEXT("RANGER") },
		{ TEXT("Demolitionist"), TEXT("爆破手"), TEXT("DEMOLITIONIST") },
		{ TEXT("Engineer"),      TEXT("工程师"), TEXT("ENGINEER") },
	};

	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Specs); ++Index)
	{
		const FProfessionSpec& Spec = Specs[Index];

		// 固定宽度让三个按钮等宽
		USizeBox* CardSize = WidgetTree->ConstructWidget<USizeBox>();
		CardSize->SetWidthOverride(280.0f);
		CardSize->SetHeightOverride(140.0f);

		UProfessionCardButton* CardButton = WidgetTree->ConstructWidget<UProfessionCardButton>();
		CardButton->InitializeCard(Spec.Id);
		CardButton->OnProfessionCardClicked.AddDynamic(this, &UWeaponClassSelectWidget::HandleProfessionCardClicked);
		{
			FButtonStyle Style;
			Style.SetNormal(MakeFlatBrush(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f)));
			Style.SetHovered(MakeFlatBrush(FLinearColor(1.0f, 1.0f, 1.0f, 0.04f)));
			Style.SetPressed(MakeFlatBrush(FLinearColor(0.780f, 0.525f, 0.098f, 0.15f)));
			Style.SetDisabled(MakeFlatBrush(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f)));
			Style.SetNormalPadding(FMargin(0.0f));
			Style.SetPressedPadding(FMargin(0.0f));
			CardButton->SetStyle(Style);
		}
		CardSize->AddChild(CardButton);

		// 按钮内容：Border（用于选中态描边与底色）+ 文本
		UBorder* CardBorder = WidgetTree->ConstructWidget<UBorder>();
		CardBorder->SetBrush(MakeOutlinedBrush(CardDefault, AmberFaint, 1.0f));
		CardBorder->SetPadding(FMargin(24.0f, 28.0f));
		ProfessionCardBorders.Add(Spec.Id, CardBorder);

		UVerticalBox* CardColumn = WidgetTree->ConstructWidget<UVerticalBox>();
		CardBorder->AddChild(CardColumn);

		{
			USpacer* TopGap = WidgetTree->ConstructWidget<USpacer>();
			UVerticalBoxSlot* GapSlot = CardColumn->AddChildToVerticalBox(TopGap);
			GapSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}

		{
			UTextBlock* NameText = MakeText(Spec.Name, 26, TextPrimary, true, 80);
			NameText->SetJustification(ETextJustify::Center);
			UVerticalBoxSlot* NameSlot = CardColumn->AddChildToVerticalBox(NameText);
			NameSlot->SetHorizontalAlignment(HAlign_Center);
		}

		{
			UTextBlock* SubText = MakeText(Spec.Subtitle, 9, TextMuted, false, 320);
			SubText->SetJustification(ETextJustify::Center);
			UVerticalBoxSlot* SubSlot = CardColumn->AddChildToVerticalBox(SubText);
			SubSlot->SetHorizontalAlignment(HAlign_Center);
			SubSlot->SetPadding(FMargin(0.0f, 6.0f, 0.0f, 0.0f));
		}

		{
			USpacer* BottomGap = WidgetTree->ConstructWidget<USpacer>();
			UVerticalBoxSlot* GapSlot = CardColumn->AddChildToVerticalBox(BottomGap);
			GapSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		}

		CardButton->AddChild(CardBorder);
		if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(CardButton->GetContentSlot()))
		{
			ContentSlot->SetPadding(FMargin(0.0f));
			ContentSlot->SetHorizontalAlignment(HAlign_Fill);
			ContentSlot->SetVerticalAlignment(VAlign_Fill);
		}

		UHorizontalBoxSlot* CardSlot = Row->AddChildToHorizontalBox(CardSize);
		CardSlot->SetPadding(FMargin(Index == 0 ? 0.0f : 18.0f, 0.0f, 0.0f, 0.0f));
		CardSlot->SetVerticalAlignment(VAlign_Center);
	}

	return Row;
}

UWidget* UWeaponClassSelectWidget::BuildStartGameButton()
{
	using namespace WeaponClassSelectStyle;

	USizeBox* ButtonSize = WidgetTree->ConstructWidget<USizeBox>();
	ButtonSize->SetWidthOverride(400.0f);

	UButton* Button = WidgetTree->ConstructWidget<UButton>();
	StartGameButton = Button;
	{
		FButtonStyle Style;
		Style.SetNormal(MakeFlatBrush(FLinearColor(0.129f, 0.145f, 0.169f, 1.0f)));
		Style.SetHovered(MakeFlatBrush(Amber));
		Style.SetPressed(MakeFlatBrush(FLinearColor(0.780f, 0.525f, 0.098f, 1.0f)));
		Style.SetDisabled(MakeFlatBrush(FLinearColor(0.08f, 0.08f, 0.08f, 0.5f)));
		Style.SetNormalPadding(FMargin(0.0f));
		Style.SetPressedPadding(FMargin(0.0f));
		Button->SetStyle(Style);
	}
	Button->OnClicked.AddDynamic(this, &UWeaponClassSelectWidget::HandleStartGameClicked);
	ButtonSize->AddChild(Button);

	UBorder* ButtonSurface = WidgetTree->ConstructWidget<UBorder>();
	ButtonSurface->SetBrushColor(FLinearColor(0.0f, 0.0f, 0.0f, 0.0f));
	ButtonSurface->SetPadding(FMargin(28.0f, 18.0f));

	UVerticalBox* ButtonColumn = WidgetTree->ConstructWidget<UVerticalBox>();
	ButtonSurface->AddChild(ButtonColumn);

	{
		UTextBlock* MainText = MakeText(TEXT("开始游戏"), 22, TextPrimary, true, 120);
		MainText->SetJustification(ETextJustify::Center);
		UVerticalBoxSlot* MainSlot = ButtonColumn->AddChildToVerticalBox(MainText);
		MainSlot->SetHorizontalAlignment(HAlign_Center);
	}

	{
		UTextBlock* SubText = MakeText(TEXT("START MISSION"), 9, TextMuted, false, 360);
		SubText->SetJustification(ETextJustify::Center);
		UVerticalBoxSlot* SubSlot = ButtonColumn->AddChildToVerticalBox(SubText);
		SubSlot->SetHorizontalAlignment(HAlign_Center);
		SubSlot->SetPadding(FMargin(0.0f, 5.0f, 0.0f, 0.0f));
	}

	Button->AddChild(ButtonSurface);
	if (UButtonSlot* ContentSlot = Cast<UButtonSlot>(Button->GetContentSlot()))
	{
		ContentSlot->SetPadding(FMargin(0.0f));
		ContentSlot->SetHorizontalAlignment(HAlign_Fill);
		ContentSlot->SetVerticalAlignment(VAlign_Fill);
	}

	// 初始状态：未选职业时禁用
	RefreshStartGameButtonState();

	return ButtonSize;
}

void UWeaponClassSelectWidget::SelectProfession(FName ProfessionId)
{
	SelectedProfessionId = ProfessionId;
	RefreshProfessionHighlight();
	RefreshStartGameButtonState();
}

void UWeaponClassSelectWidget::HandleProfessionCardClicked(FName ProfessionId)
{
	SelectProfession(ProfessionId);
}

void UWeaponClassSelectWidget::HandleStartGameClicked()
{
	if (SelectedProfessionId.IsNone())
	{
		return;
	}

	// 解析默认武器：优先 GetDefaultProfessionWeaponId，回退取池第一项
	UGameInstance* GameInstance = GetGameInstance();
	UGameDataSubsystem* DataSys = GameInstance ? GameInstance->GetSubsystem<UGameDataSubsystem>() : nullptr;

	FName PrimaryWeaponId = DataSys ? DataSys->GetDefaultProfessionWeaponId(SelectedProfessionId, TEXT("Primary")) : NAME_None;
	if (PrimaryWeaponId.IsNone() && DataSys)
	{
		const TArray<FName> PrimaryPool = DataSys->GetProfessionWeaponIds(SelectedProfessionId, TEXT("Primary"));
		if (PrimaryPool.Num() > 0)
		{
			PrimaryWeaponId = PrimaryPool[0];
		}
	}

	FName SecondaryWeaponId = DataSys ? DataSys->GetDefaultProfessionWeaponId(SelectedProfessionId, TEXT("Secondary")) : NAME_None;
	if (SecondaryWeaponId.IsNone() && DataSys)
	{
		const TArray<FName> SecondaryPool = DataSys->GetProfessionWeaponIds(SelectedProfessionId, TEXT("Secondary"));
		if (SecondaryPool.Num() > 0)
		{
			SecondaryWeaponId = SecondaryPool[0];
		}
	}

	// 调用 PlayerController::RequestSelectProfession，签名不变
	APlayerController* PC = GetOwningPlayer();
	if (AWeaponPlayerController* WeaponPC = Cast<AWeaponPlayerController>(PC))
	{
		WeaponPC->RequestSelectProfession(SelectedProfessionId, PrimaryWeaponId, SecondaryWeaponId);
	}
}

void UWeaponClassSelectWidget::RefreshProfessionHighlight()
{
	using namespace WeaponClassSelectStyle;

	for (const TPair<FName, TObjectPtr<UBorder>>& Pair : ProfessionCardBorders)
	{
		if (UBorder* CardBorder = Pair.Value)
		{
			if (Pair.Key == SelectedProfessionId)
			{
				// 选中：橙黄描边 + 抬起底色
				CardBorder->SetBrush(MakeOutlinedBrush(CardSelected, Amber, 2.0f));
			}
			else
			{
				// 未选中：暗色底 + 低亮描边
				CardBorder->SetBrush(MakeOutlinedBrush(CardDefault, AmberFaint, 1.0f));
			}
		}
	}
}

void UWeaponClassSelectWidget::RefreshStartGameButtonState()
{
	if (!StartGameButton)
	{
		return;
	}

	const bool bHasSelection = !SelectedProfessionId.IsNone();
	StartGameButton->SetIsEnabled(bHasSelection);
	StartGameButton->SetRenderOpacity(bHasSelection ? 1.0f : 0.4f);
}

UTextBlock* UWeaponClassSelectWidget::MakeText(
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
