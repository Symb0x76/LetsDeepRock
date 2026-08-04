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
#include "Components/WidgetSwitcher.h"
#include "Components/WidgetSwitcherSlot.h"
#include "Styling/CoreStyle.h"
#include "Data/GameDataSubsystem.h"
#include "Engine/GameInstance.h"

namespace WeaponClassSelectWidgetPrivate
{
	// 选中态 / 未选中态卡片边框颜色（沿用现有矿业终端配色语言）
	static const FLinearColor CardColorDefault(0.15f, 0.2f, 0.25f, 0.95f);
	static const FLinearColor CardColorSelected(0.35f, 0.55f, 0.35f, 0.95f); // 选中：偏绿高亮，与职业卡片琥珀色文字区分
	static const FLinearColor AccentColor(1.0f, 0.85f, 0.3f, 1.0f);
	static const FLinearColor TextSecondaryColor(0.7f, 0.75f, 0.8f, 1.0f);
}

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

	// 两页切换容器：Page0 职业选择，Page1 武器选择
	PageSwitcher = WidgetTree->ConstructWidget<UWidgetSwitcher>();
	SafeArea->AddChild(PageSwitcher);
	PageSwitcher->SetActiveWidgetIndex(ProfessionPageIndex);

	// ================= Page 0：职业选择（沿用原有布局与视觉） =================

	UVerticalBox* ProfessionPageLayout = WidgetTree->ConstructWidget<UVerticalBox>();
	PageSwitcher->AddChild(ProfessionPageLayout);

	// 上部 Spacer（占 30% 垂直空间，标题区域）
	USpacer* TopSpacer = WidgetTree->ConstructWidget<USpacer>();
	UVerticalBoxSlot* TopSpacerVBoxSlot = ProfessionPageLayout->AddChildToVerticalBox(TopSpacer);
	TopSpacerVBoxSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	TopSpacerVBoxSlot->SetVerticalAlignment(VAlign_Fill);

	// 中文标题
	UTextBlock* TitleText = WidgetTree->ConstructWidget<UTextBlock>();
	TitleText->SetText(FText::FromString(TEXT("选择职业")));
	TitleText->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 42));
	TitleText->SetColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.95f, 1.0f, 1.0f)));
	TitleText->SetJustification(ETextJustify::Center);
	UVerticalBoxSlot* TitleVBoxSlot = ProfessionPageLayout->AddChildToVerticalBox(TitleText);
	TitleVBoxSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 40.f));
	TitleVBoxSlot->SetHorizontalAlignment(HAlign_Center);

	// 卡片容器：ScaleBox 自适应缩放 + SizeBox 限定设计尺寸
	UScaleBox* ScaleContainer = WidgetTree->ConstructWidget<UScaleBox>();
	ScaleContainer->SetStretch(EStretch::ScaleToFit); // 同时受可用宽高约束，避免窄屏或低分辨率裁切
	UVerticalBoxSlot* ScaleVBoxSlot = ProfessionPageLayout->AddChildToVerticalBox(ScaleContainer);
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
		CardBorderWidget->SetBrushColor(WeaponClassSelectWidgetPrivate::CardColorDefault);
		CardBorderWidget->SetPadding(FMargin(25.f, 35.f));

		UVerticalBox* CardContentBox = WidgetTree->ConstructWidget<UVerticalBox>();

		UTextBlock* ProfessionNameText = WidgetTree->ConstructWidget<UTextBlock>();
		ProfessionNameText->SetText(CardData.DisplayName);
		ProfessionNameText->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 32));
		ProfessionNameText->SetColorAndOpacity(FSlateColor(WeaponClassSelectWidgetPrivate::AccentColor));
		ProfessionNameText->SetJustification(ETextJustify::Center);

		UTextBlock* ProfessionDescText = WidgetTree->ConstructWidget<UTextBlock>();
		ProfessionDescText->SetText(CardData.Description);
		ProfessionDescText->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Regular"), 18));
		ProfessionDescText->SetColorAndOpacity(FSlateColor(WeaponClassSelectWidgetPrivate::TextSecondaryColor));
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
	UVerticalBoxSlot* BottomSpacerVBoxSlot = ProfessionPageLayout->AddChildToVerticalBox(BottomSpacer);
	BottomSpacerVBoxSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	BottomSpacerVBoxSlot->SetVerticalAlignment(VAlign_Fill);

	// ================= Page 1：武器选择（内容延迟到 SelectProfession 时动态构建） =================

	WeaponPageRoot = WidgetTree->ConstructWidget<UVerticalBox>();
	PageSwitcher->AddChild(WeaponPageRoot);
}

void UWeaponClassSelectWidget::SelectProfession(FName ProfessionId)
{
	// 仅记录职业选择并进入武器选择页，不在此处触发重生。
	SelectedProfessionId = ProfessionId;
	SelectedPrimaryWeaponId = NAME_None;
	SelectedSecondaryWeaponId = NAME_None;

	BuildWeaponSelectPage();

	if (PageSwitcher)
	{
		PageSwitcher->SetActiveWidgetIndex(WeaponPageIndex);
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

void UWeaponClassSelectWidget::BuildWeaponSelectPage()
{
	using namespace WeaponClassSelectWidgetPrivate;

	if (!WeaponPageRoot || !WidgetTree)
	{
		return;
	}

	// 每次重新进入武器页都从空白重建（不同职业武器池不同，避免残留旧卡片）
	WeaponPageRoot->ClearChildren();
	PrimaryWeaponCardBorders.Reset();
	SecondaryWeaponCardBorders.Reset();
	StartGameButton = nullptr;

	UGameDataSubsystem* DataSys = nullptr;
	if (UGameInstance* GI = GetGameInstance())
	{
		DataSys = GI->GetSubsystem<UGameDataSubsystem>();
	}

	// 上部 Spacer，与职业页保持一致的垂直节奏
	USpacer* TopSpacer = WidgetTree->ConstructWidget<USpacer>();
	UVerticalBoxSlot* TopSpacerSlot = WeaponPageRoot->AddChildToVerticalBox(TopSpacer);
	TopSpacerSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	TopSpacerSlot->SetVerticalAlignment(VAlign_Fill);

	// 标题
	UTextBlock* TitleText = WidgetTree->ConstructWidget<UTextBlock>();
	TitleText->SetText(FText::FromString(TEXT("选择武器装备")));
	TitleText->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 36));
	TitleText->SetColorAndOpacity(FSlateColor(FLinearColor(0.9f, 0.95f, 1.0f, 1.0f)));
	TitleText->SetJustification(ETextJustify::Center);
	UVerticalBoxSlot* TitleSlot = WeaponPageRoot->AddChildToVerticalBox(TitleText);
	TitleSlot->SetPadding(FMargin(0.f, 0.f, 0.f, 30.f));
	TitleSlot->SetHorizontalAlignment(HAlign_Center);

	// 卡片区域：ScaleBox 自适应缩放 + SizeBox 限定设计尺寸（与职业页手法一致）
	UScaleBox* ScaleContainer = WidgetTree->ConstructWidget<UScaleBox>();
	ScaleContainer->SetStretch(EStretch::ScaleToFit);
	UVerticalBoxSlot* ScaleSlot = WeaponPageRoot->AddChildToVerticalBox(ScaleContainer);
	ScaleSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	ScaleSlot->SetHorizontalAlignment(HAlign_Center);
	ScaleSlot->SetVerticalAlignment(VAlign_Center);

	USizeBox* DesignSizeBox = WidgetTree->ConstructWidget<USizeBox>();
	DesignSizeBox->SetWidthOverride(1200.f);
	DesignSizeBox->SetHeightOverride(420.f);
	ScaleContainer->AddChild(DesignSizeBox);

	UVerticalBox* SlotsLayout = WidgetTree->ConstructWidget<UVerticalBox>();
	DesignSizeBox->AddChild(SlotsLayout);

	// 依次构建 Primary / Secondary 两组槽位
	struct FSlotBuildInfo
	{
		FName SlotType;
		FText SlotLabel;
		TMap<FName, TObjectPtr<UBorder>>* CardBorderMap;
	};

	const FSlotBuildInfo SlotInfos[] =
	{
		{ TEXT("Primary"), FText::FromString(TEXT("主武器")), &PrimaryWeaponCardBorders },
		{ TEXT("Secondary"), FText::FromString(TEXT("副武器")), &SecondaryWeaponCardBorders },
	};

	for (int32 SlotIdx = 0; SlotIdx < UE_ARRAY_COUNT(SlotInfos); ++SlotIdx)
	{
		const FSlotBuildInfo& SlotInfo = SlotInfos[SlotIdx];

		// 槽位标签
		UTextBlock* SlotLabelText = WidgetTree->ConstructWidget<UTextBlock>();
		SlotLabelText->SetText(SlotInfo.SlotLabel);
		SlotLabelText->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 22));
		SlotLabelText->SetColorAndOpacity(FSlateColor(AccentColor));
		UVerticalBoxSlot* SlotLabelVBoxSlot = SlotsLayout->AddChildToVerticalBox(SlotLabelText);
		SlotLabelVBoxSlot->SetPadding(FMargin(0.f, SlotIdx == 0 ? 0.f : 20.f, 0.f, 10.f));

		// 该槽位合法武器列表
		const TArray<FName> WeaponIds = DataSys ? DataSys->GetProfessionWeaponIds(SelectedProfessionId, SlotInfo.SlotType) : TArray<FName>();
		const FName DefaultWeaponId = DataSys ? DataSys->GetDefaultProfessionWeaponId(SelectedProfessionId, SlotInfo.SlotType) : NAME_None;

		UHorizontalBox* WeaponCardsRow = WidgetTree->ConstructWidget<UHorizontalBox>();
		UVerticalBoxSlot* WeaponCardsRowSlot = SlotsLayout->AddChildToVerticalBox(WeaponCardsRow);
		WeaponCardsRowSlot->SetHorizontalAlignment(HAlign_Fill);

		for (int32 WeaponIdx = 0; WeaponIdx < WeaponIds.Num(); ++WeaponIdx)
		{
			const FName WeaponId = WeaponIds[WeaponIdx];

			// 武器展示名：优先取 DT_WeaponConfig.DisplayName，取不到时回退 WeaponId 本身
			FText WeaponDisplayName = FText::FromName(WeaponId);
			if (DataSys)
			{
				if (const FWeaponConfigRow* WeaponRow = DataSys->GetWeaponConfig(WeaponId))
				{
					if (!WeaponRow->DisplayName.IsEmpty())
					{
						WeaponDisplayName = WeaponRow->DisplayName;
					}
				}
			}

			UWeaponCardButton* CardButton = WidgetTree->ConstructWidget<UWeaponCardButton>();
			// 显式初始化：设置槽位/武器 Id 并确保 OnClicked 只绑定一次
			// （UWeaponCardButton 继承自 UButton，NativeOnInitialized 在此处不会被引擎自动调用，
			// 因此不依赖它做绑定，改为构造后立即显式调用）。
			CardButton->InitializeCard(SlotInfo.SlotType, WeaponId);
			// 动态数量按钮无法各自声明独立 UFUNCTION，统一订阅到 HandleWeaponCardClicked，
			// 由 UWeaponCardButton 点击时自行广播 SlotType/WeaponId。
			CardButton->OnWeaponCardClicked.AddDynamic(this, &UWeaponClassSelectWidget::HandleWeaponCardClicked);

			UBorder* CardBorderWidget = WidgetTree->ConstructWidget<UBorder>();
			CardBorderWidget->SetBrushColor(CardColorDefault);
			CardBorderWidget->SetPadding(FMargin(20.f, 25.f));

			UTextBlock* WeaponNameText = WidgetTree->ConstructWidget<UTextBlock>();
			WeaponNameText->SetText(WeaponDisplayName);
			WeaponNameText->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 20));
			WeaponNameText->SetColorAndOpacity(FSlateColor(FLinearColor(0.95f, 0.95f, 0.95f, 1.0f)));
			WeaponNameText->SetJustification(ETextJustify::Center);
			WeaponNameText->SetAutoWrapText(true);

			CardBorderWidget->AddChild(WeaponNameText);
			CardButton->AddChild(CardBorderWidget);

			UHorizontalBoxSlot* CardHBoxSlot = WeaponCardsRow->AddChildToHorizontalBox(CardButton);
			CardHBoxSlot->SetPadding(FMargin(WeaponIdx == 0 ? 0.f : 12.f, 0.f, 0.f, 0.f));
			CardHBoxSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
			CardHBoxSlot->SetHorizontalAlignment(HAlign_Fill);
			CardHBoxSlot->SetVerticalAlignment(VAlign_Fill);

			SlotInfo.CardBorderMap->Add(WeaponId, CardBorderWidget);
		}

		// 默认预选：池非空时选中 bDefaultEquipped 行；取不到默认值则回退池中第一把，保证槽位始终有效
		FName InitialSelection = DefaultWeaponId;
		if (InitialSelection.IsNone() && WeaponIds.Num() > 0)
		{
			InitialSelection = WeaponIds[0];
		}

		if (SlotInfo.SlotType == TEXT("Primary"))
		{
			SelectedPrimaryWeaponId = InitialSelection;
		}
		else
		{
			SelectedSecondaryWeaponId = InitialSelection;
		}

		RefreshWeaponSlotHighlight(*SlotInfo.CardBorderMap, InitialSelection);
	}

	// 底部按钮行：返回 + 开始游戏
	UHorizontalBox* BottomButtonsRow = WidgetTree->ConstructWidget<UHorizontalBox>();
	UVerticalBoxSlot* BottomButtonsRowSlot = SlotsLayout->AddChildToVerticalBox(BottomButtonsRow);
	BottomButtonsRowSlot->SetPadding(FMargin(0.f, 30.f, 0.f, 0.f));
	BottomButtonsRowSlot->SetHorizontalAlignment(HAlign_Fill);

	// 返回按钮
	{
		UButton* BackButton = WidgetTree->ConstructWidget<UButton>();
		UBorder* BackBorder = WidgetTree->ConstructWidget<UBorder>();
		BackBorder->SetBrushColor(FLinearColor(0.2f, 0.2f, 0.2f, 0.95f));
		BackBorder->SetPadding(FMargin(20.f, 14.f));

		UTextBlock* BackText = WidgetTree->ConstructWidget<UTextBlock>();
		BackText->SetText(FText::FromString(TEXT("返回")));
		BackText->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 20));
		BackText->SetColorAndOpacity(FSlateColor(TextSecondaryColor));
		BackText->SetJustification(ETextJustify::Center);

		BackBorder->AddChild(BackText);
		BackButton->AddChild(BackBorder);
		BackButton->OnClicked.AddDynamic(this, &UWeaponClassSelectWidget::HandleBackToProfessionClicked);

		UHorizontalBoxSlot* BackHBoxSlot = BottomButtonsRow->AddChildToHorizontalBox(BackButton);
		BackHBoxSlot->SetPadding(FMargin(0.f, 0.f, 15.f, 0.f));
		BackHBoxSlot->SetHorizontalAlignment(HAlign_Left);
		BackHBoxSlot->SetVerticalAlignment(VAlign_Center);
	}

	// 开始游戏按钮
	{
		StartGameButton = WidgetTree->ConstructWidget<UButton>();
		UBorder* StartBorder = WidgetTree->ConstructWidget<UBorder>();
		StartBorder->SetBrushColor(FLinearColor(0.3f, 0.5f, 0.3f, 0.95f));
		StartBorder->SetPadding(FMargin(30.f, 14.f));

		UTextBlock* StartText = WidgetTree->ConstructWidget<UTextBlock>();
		StartText->SetText(FText::FromString(TEXT("开始游戏")));
		StartText->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 22));
		StartText->SetColorAndOpacity(FSlateColor(FLinearColor(1.0f, 1.0f, 1.0f, 1.0f)));
		StartText->SetJustification(ETextJustify::Center);

		StartBorder->AddChild(StartText);
		StartGameButton->AddChild(StartBorder);
		StartGameButton->OnClicked.AddDynamic(this, &UWeaponClassSelectWidget::HandleStartGameClicked);

		UHorizontalBoxSlot* StartHBoxSlot = BottomButtonsRow->AddChildToHorizontalBox(StartGameButton);
		StartHBoxSlot->SetHorizontalAlignment(HAlign_Right);
		StartHBoxSlot->SetVerticalAlignment(VAlign_Center);
	}

	// 下部 Spacer，保持与职业页一致的垂直居中节奏
	USpacer* BottomSpacer = WidgetTree->ConstructWidget<USpacer>();
	UVerticalBoxSlot* BottomSpacerSlot = WeaponPageRoot->AddChildToVerticalBox(BottomSpacer);
	BottomSpacerSlot->SetSize(FSlateChildSize(ESlateSizeRule::Fill));
	BottomSpacerSlot->SetVerticalAlignment(VAlign_Fill);

	RefreshStartGameButtonState();
}

void UWeaponClassSelectWidget::HandleWeaponCardClicked(FName SlotType, FName WeaponId)
{
	if (SlotType == TEXT("Primary"))
	{
		SelectedPrimaryWeaponId = WeaponId;
		RefreshWeaponSlotHighlight(PrimaryWeaponCardBorders, WeaponId);
	}
	else if (SlotType == TEXT("Secondary"))
	{
		SelectedSecondaryWeaponId = WeaponId;
		RefreshWeaponSlotHighlight(SecondaryWeaponCardBorders, WeaponId);
	}

	RefreshStartGameButtonState();
}

void UWeaponClassSelectWidget::HandleBackToProfessionClicked()
{
	// 清空武器选择状态，回到职业选择页
	SelectedProfessionId = NAME_None;
	SelectedPrimaryWeaponId = NAME_None;
	SelectedSecondaryWeaponId = NAME_None;
	PrimaryWeaponCardBorders.Reset();
	SecondaryWeaponCardBorders.Reset();

	if (PageSwitcher)
	{
		PageSwitcher->SetActiveWidgetIndex(ProfessionPageIndex);
	}
}

void UWeaponClassSelectWidget::HandleStartGameClicked()
{
	if (SelectedPrimaryWeaponId.IsNone() || SelectedSecondaryWeaponId.IsNone())
	{
		return;
	}

	APlayerController* PC = GetOwningPlayer();
	if (AWeaponPlayerController* WeaponPC = Cast<AWeaponPlayerController>(PC))
	{
		WeaponPC->RequestSelectProfession(SelectedProfessionId, SelectedPrimaryWeaponId, SelectedSecondaryWeaponId);
	}
}

void UWeaponClassSelectWidget::RefreshWeaponSlotHighlight(const TMap<FName, TObjectPtr<UBorder>>& CardBorders, FName SelectedWeaponId)
{
	using namespace WeaponClassSelectWidgetPrivate;

	for (const TPair<FName, TObjectPtr<UBorder>>& Pair : CardBorders)
	{
		if (UBorder* CardBorder = Pair.Value)
		{
			CardBorder->SetBrushColor(Pair.Key == SelectedWeaponId ? CardColorSelected : CardColorDefault);
		}
	}
}

void UWeaponClassSelectWidget::RefreshStartGameButtonState()
{
	if (!StartGameButton)
	{
		return;
	}

	const bool bBothSlotsValid = !SelectedPrimaryWeaponId.IsNone() && !SelectedSecondaryWeaponId.IsNone();
	StartGameButton->SetIsEnabled(bBothSlotsValid);
	StartGameButton->SetRenderOpacity(bBothSlotsValid ? 1.0f : 0.4f);
}
