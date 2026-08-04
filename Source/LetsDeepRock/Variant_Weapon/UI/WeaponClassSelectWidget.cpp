// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponClassSelectWidget.h"
#include "GameFramework/PlayerController.h"
#include "WeaponPlayerController.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Button.h"
#include "Components/TextBlock.h"
#include "Components/Border.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/SafeZone.h"
#include "Components/SafeZoneSlot.h"
#include "Components/ScaleBox.h"
#include "Components/ScaleBoxSlot.h"
#include "Components/SizeBox.h"
#include "Components/SizeBoxSlot.h"
#include "Components/Spacer.h"
#include "Styling/CoreStyle.h"

void UWeaponClassSelectWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"));
	}

	// 根画布
	UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(GetRootWidget());
	if (!RootCanvas)
	{
		RootCanvas = NewObject<UCanvasPanel>(this);
		WidgetTree->RootWidget = RootCanvas;
	}

	// 全屏半透明遮罩（矿业科幻终端深色背景）
	UBorder* FullScreenOverlay = WidgetTree->ConstructWidget<UBorder>();
	FullScreenOverlay->SetBrushColor(FLinearColor(0.05f, 0.1f, 0.12f, 0.92f));
	UCanvasPanelSlot* OverlayCanvasSlot = RootCanvas->AddChildToCanvas(FullScreenOverlay);
	OverlayCanvasSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
	OverlayCanvasSlot->SetOffsets(FMargin(0.f));

	// SafeZone 确保内容不被屏幕边缘或异形屏裁切
	USafeZone* SafeArea = WidgetTree->ConstructWidget<USafeZone>();
	UCanvasPanelSlot* SafeCanvasSlot = RootCanvas->AddChildToCanvas(SafeArea);
	SafeCanvasSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));
	SafeCanvasSlot->SetOffsets(FMargin(20.f, 20.f, -20.f, -20.f)); // 额外保留 20px 屏幕边距

	// 垂直布局：标题在上，卡片容器居中偏下（避开左上角 HUD）
	UVerticalBox* MainLayout = WidgetTree->ConstructWidget<UVerticalBox>();
	USafeZoneSlot* SafeContentSlot = Cast<USafeZoneSlot>(SafeArea->AddChild(MainLayout));
	if (SafeContentSlot)
	{
		SafeContentSlot->SetHorizontalAlignment(HAlign_Fill);
		SafeContentSlot->SetVerticalAlignment(VAlign_Fill);
	}

	// 上部 Spacer（占 30% 垂直空间，标题区域）
	USpacer* TopSpacer = WidgetTree->ConstructWidget<USpacer>();
	UVerticalBoxSlot* TopSpacerVBoxSlot = MainLayout->AddChildToVerticalBox(TopSpacer);
	TopSpacerVBoxSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	TopSpacerVBoxSlot->SetVerticalAlignment(VAlign_Fill);

	// 中文标题
	UTextBlock* TitleText = WidgetTree->ConstructWidget<UTextBlock>();
	TitleText->SetText(FText::FromString(TEXT("选择职业")));
	TitleText->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 42));
	TitleText->SetColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.95f, 1.0f, 1.0f)));
	TitleText->SetJustification(ETextJustify::Center);
	UVerticalBoxSlot* TitleVBoxSlot = MainLayout->AddChildToVerticalBox(TitleText);
	TitleVBoxSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 40.f));
	TitleVBoxSlot->SetHorizontalAlignment(HAlign_Center);

	// 卡片容器：ScaleBox 自适应缩放 + SizeBox 限定设计尺寸
	UScaleBox* ScaleContainer = WidgetTree->ConstructWidget<UScaleBox>();
	ScaleContainer->SetStretch(EStretch::ScaleToFit); // 同时受可用宽高约束，避免窄屏或低分辨率裁切
	UVerticalBoxSlot* ScaleVBoxSlot = MainLayout->AddChildToVerticalBox(ScaleContainer);
	ScaleVBoxSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	ScaleVBoxSlot->SetHorizontalAlignment(HAlign_Center);
	ScaleVBoxSlot->SetVerticalAlignment(VAlign_Center);

	// SizeBox 定义卡片面板的设计尺寸（1200x350 作为基准，ScaleBox 会按可用空间等比缩放）
	USizeBox* DesignSizeBox = WidgetTree->ConstructWidget<USizeBox>();
	DesignSizeBox->SetWidthOverride(1200.f);
	DesignSizeBox->SetHeightOverride(350.f);
	ScaleContainer->AddChild(DesignSizeBox);

	// 横向三个职业卡片容器
	UHorizontalBox* CardsContainer = WidgetTree->ConstructWidget<UHorizontalBox>();
	DesignSizeBox->AddChild(CardsContainer);

	// 职业数据（ProfessionId 必须与 DT_ProfessionConfig 的 RowName 一致）
	struct FProfessionCardData
	{
		FName ProfessionID;
		FText DisplayName;
		FText Description;
		FScriptDelegate ClickDelegate;
	};

	TArray<FProfessionCardData> ProfessionCards;

	// 游侠
	FProfessionCardData RangerCard;
	RangerCard.ProfessionID = TEXT("Ranger");
	RangerCard.DisplayName = FText::FromString(TEXT("游侠"));
	RangerCard.Description = FText::FromString(TEXT("远程精准打击 · 元素手雷"));
	RangerCard.ClickDelegate.BindUFunction(this, FName("OnRangerClicked"));
	ProfessionCards.Add(RangerCard);

	// 爆破手
	FProfessionCardData DemolitionistCard;
	DemolitionistCard.ProfessionID = TEXT("Demolitionist");
	DemolitionistCard.DisplayName = FText::FromString(TEXT("爆破手"));
	DemolitionistCard.Description = FText::FromString(TEXT("范围压制 · 火焰与近距离火力"));
	DemolitionistCard.ClickDelegate.BindUFunction(this, FName("OnDemolitionistClicked"));
	ProfessionCards.Add(DemolitionistCard);

	// 工程师
	FProfessionCardData EngineerCard;
	EngineerCard.ProfessionID = TEXT("Engineer");
	EngineerCard.DisplayName = FText::FromString(TEXT("工程师"));
	EngineerCard.Description = FText::FromString(TEXT("持续火力 · 战场控制"));
	EngineerCard.ClickDelegate.BindUFunction(this, FName("OnEngineerClicked"));
	ProfessionCards.Add(EngineerCard);

	// 创建三张等宽职业卡片
	for (int32 Idx = 0; Idx < ProfessionCards.Num(); ++Idx)
	{
		const FProfessionCardData& CardData = ProfessionCards[Idx];

		UButton* CardButton = WidgetTree->ConstructWidget<UButton>();

		// 按钮内容：边框包裹垂直布局（职业名 + 简介）
		UBorder* CardBorderWidget = WidgetTree->ConstructWidget<UBorder>();
		CardBorderWidget->SetBrushColor(FLinearColor(0.15f, 0.2f, 0.25f, 0.95f));
		CardBorderWidget->SetPadding(FMargin(25.f, 35.f));

		UVerticalBox* CardContentBox = WidgetTree->ConstructWidget<UVerticalBox>();

		UTextBlock* ProfessionNameText = WidgetTree->ConstructWidget<UTextBlock>();
		ProfessionNameText->SetText(CardData.DisplayName);
		ProfessionNameText->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 32));
		ProfessionNameText->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 0.85f, 0.3f, 1.0f)));
		ProfessionNameText->SetJustification(ETextJustify::Center);

		UTextBlock* ProfessionDescText = WidgetTree->ConstructWidget<UTextBlock>();
		ProfessionDescText->SetText(CardData.Description);
		ProfessionDescText->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 18));
		ProfessionDescText->SetColorAndOpacity(FSlateColor(FLinearColor(0.7f, 0.75f, 0.8f, 1.0f)));
		ProfessionDescText->SetJustification(ETextJustify::Center);
		ProfessionDescText->SetAutoWrapText(true);

		UVerticalBoxSlot* NameCardVBoxSlot = CardContentBox->AddChildToVerticalBox(ProfessionNameText);
		NameCardVBoxSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 20.f));

		UVerticalBoxSlot* DescCardVBoxSlot = CardContentBox->AddChildToVerticalBox(ProfessionDescText);
		DescCardVBoxSlot->SetPadding(FMargin(0.f));

		CardBorderWidget->AddChild(CardContentBox);
		CardButton->AddChild(CardBorderWidget);

		// 绑定点击事件
		CardButton->OnClicked.Add(CardData.ClickDelegate);

		// 添加到横向容器（三张卡片等宽 Fill）
		UHorizontalBoxSlot* CardContainerHBoxSlot = CardsContainer->AddChildToHorizontalBox(CardButton);
		CardContainerHBoxSlot->SetPadding(FMargin(Idx == 0 ? 0.f : 15.f, 0.f, 0.f, 0.f)); // 第一张左侧无间距
		CardContainerHBoxSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
		CardContainerHBoxSlot->SetHorizontalAlignment(HAlign_Fill);
		CardContainerHBoxSlot->SetVerticalAlignment(VAlign_Fill);
	}

	// 下部 Spacer（占剩余空间，让卡片区域垂直居中偏下）
	USpacer* BottomSpacer = WidgetTree->ConstructWidget<USpacer>();
	UVerticalBoxSlot* BottomSpacerVBoxSlot = MainLayout->AddChildToVerticalBox(BottomSpacer);
	BottomSpacerVBoxSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	BottomSpacerVBoxSlot->SetVerticalAlignment(VAlign_Fill);
}

void UWeaponClassSelectWidget::SelectProfession(FName ProfessionId)
{
	APlayerController* PC = GetOwningPlayer();
	if (AWeaponPlayerController* WeaponPC = Cast<AWeaponPlayerController>(PC))
	{
		WeaponPC->RequestSelectProfession(ProfessionId);
	}
}

void UWeaponClassSelectWidget::OnRangerClicked()
{
	SelectProfession(TEXT("Ranger"));
}

void UWeaponClassSelectWidget::OnDemolitionistClicked()
{
	SelectProfession(TEXT("Demolitionist"));
}

void UWeaponClassSelectWidget::OnEngineerClicked()
{
	SelectProfession(TEXT("Engineer"));
}
