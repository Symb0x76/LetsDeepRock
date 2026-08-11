// 网格房间模板编辑器 - 资产编辑器 Toolkit

#include "GridMapRoomEditorToolkit.h"

#include "Editor.h"
#include "Framework/Application/SlateApplication.h"
#include "Framework/Commands/GenericCommands.h"
#include "Framework/Docking/TabManager.h"
#include "Framework/Notifications/NotificationManager.h"
#include "GridMap/GridMapRoomAsset.h"
#include "Modules/ModuleManager.h"
#include "PropertyEditorModule.h"
#include "SGridMapRoomCanvas.h"
#include "ScopedTransaction.h"
#include "Styling/AppStyle.h"
#include "Widgets/Docking/SDockTab.h"
#include "Widgets/Images/SImage.h"
#include "Widgets/Input/SButton.h"
#include "Widgets/Input/SCheckBox.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SScrollBox.h"
#include "Widgets/Layout/SSeparator.h"
#include "Widgets/Layout/SWrapBox.h"
#include "Widgets/Notifications/SNotificationList.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/Text/STextBlock.h"
#include "Widgets/Views/SListView.h"
#include "Widgets/Views/STableRow.h"

#define LOCTEXT_NAMESPACE "GridMapRoomEditor"

namespace GridMapRoomToolkitLocal
{
	static constexpr int32 MaxRoomDimension = 512;
	static const FName AppIdentifier(TEXT("GridMapRoomEditorApp"));
	static const FName CanvasTabId(TEXT("GridMapRoomEditor_Canvas"));
	static const FName PaletteTabId(TEXT("GridMapRoomEditor_Palette"));
	static const FName DetailsTabId(TEXT("GridMapRoomEditor_Details"));
	static const FName IssuesTabId(TEXT("GridMapRoomEditor_Issues"));

	FText SeverityLabel(const EGridMapValidationSeverity Severity)
	{
		return Severity == EGridMapValidationSeverity::Error
			? LOCTEXT("SeverityError", "错误")
			: LOCTEXT("SeverityWarning", "警告");
	}

	FLinearColor SeverityColor(const EGridMapValidationSeverity Severity)
	{
		return Severity == EGridMapValidationSeverity::Error
			? FLinearColor(0.925f, 0.325f, 0.271f)
			: FLinearColor(0.976f, 0.729f, 0.196f);
	}
}

// ---------------------------------------------------------------------------
// 生命周期
// ---------------------------------------------------------------------------

FGridMapRoomEditorToolkit::~FGridMapRoomEditorToolkit()
{
	if (GEditor)
	{
		GEditor->UnregisterForUndo(this);
	}
}

void FGridMapRoomEditorToolkit::InitEditor(const EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, UGridMapRoomAsset* InRoom)
{
	using namespace GridMapRoomToolkitLocal;

	if (InRoom)
	{
		LastKnownStatus = InRoom->Status;
		LastKnownSize = InRoom->Size;
	}

	// Details 面板
	FPropertyEditorModule& PropertyEditorModule = FModuleManager::LoadModuleChecked<FPropertyEditorModule>("PropertyEditor");
	FDetailsViewArgs DetailsViewArgs;
	DetailsViewArgs.NameAreaSettings = FDetailsViewArgs::HideNameArea;
	DetailsViewArgs.bHideSelectionTip = true;
	DetailsViewArgs.bAllowSearch = true;
	DetailsView = PropertyEditorModule.CreateDetailView(DetailsViewArgs);
	DetailsView->SetObject(InRoom);
	DetailsView->OnFinishedChangingProperties().AddSP(this, &FGridMapRoomEditorToolkit::HandlePropertyChanged);

	// 画布
	CanvasWidget = SNew(SGridMapRoomCanvas)
		.Room(InRoom)
		.ActiveTool_Lambda([this] { return ActiveTool; })
		.PaintTile_Lambda([this] { return ActiveTile; })
		.MarkerType_Lambda([this] { return ActiveMarker; })
		.BrushSize_Lambda([this] { return ActiveBrushSize; })
		.ShowReachability_Lambda([this] { return bShowReachability; })
		.OnRoomModified(SGridMapRoomCanvas::FOnRoomModified::CreateSP(this, &FGridMapRoomEditorToolkit::HandleRoomModified))
		.OnTilePicked(SGridMapRoomCanvas::FOnTilePicked::CreateSP(this, &FGridMapRoomEditorToolkit::HandleTilePicked))
		.OnCanvasSelectionChanged(SGridMapRoomCanvas::FOnCanvasSelectionChanged::CreateSP(this, &FGridMapRoomEditorToolkit::HandleCanvasSelectionChanged));

	const TSharedRef<FTabManager::FLayout> Layout = FTabManager::NewLayout("GridMapRoomEditor_Layout_v1")
	->AddArea
	(
		FTabManager::NewPrimaryArea()->SetOrientation(Orient_Vertical)
		->Split
		(
			FTabManager::NewSplitter()->SetOrientation(Orient_Horizontal)->SetSizeCoefficient(0.78f)
			->Split
			(
				FTabManager::NewStack()->SetSizeCoefficient(0.17f)
				->AddTab(PaletteTabId, ETabState::OpenedTab)
				->SetHideTabWell(true)
			)
			->Split
			(
				FTabManager::NewStack()->SetSizeCoefficient(0.58f)
				->AddTab(CanvasTabId, ETabState::OpenedTab)
				->SetHideTabWell(true)
			)
			->Split
			(
				FTabManager::NewStack()->SetSizeCoefficient(0.25f)
				->AddTab(DetailsTabId, ETabState::OpenedTab)
			)
		)
		->Split
		(
			FTabManager::NewStack()->SetSizeCoefficient(0.22f)
			->AddTab(IssuesTabId, ETabState::OpenedTab)
		)
	);

	const bool bCreateDefaultStandaloneMenu = true;
	const bool bCreateDefaultToolbar = true;
	FAssetEditorToolkit::InitAssetEditor(Mode, InitToolkitHost, AppIdentifier, Layout,
		bCreateDefaultStandaloneMenu, bCreateDefaultToolbar, InRoom);

	// Ctrl+Z / Ctrl+Y
	FGenericCommands::Register();
	ToolkitCommands->MapAction(FGenericCommands::Get().Undo,
		FExecuteAction::CreateLambda([] { if (GEditor) { GEditor->UndoTransaction(); } }));
	ToolkitCommands->MapAction(FGenericCommands::Get().Redo,
		FExecuteAction::CreateLambda([] { if (GEditor) { GEditor->RedoTransaction(); } }));

	if (GEditor)
	{
		GEditor->RegisterForUndo(this);
	}

	RunValidation();
}

UGridMapRoomAsset* FGridMapRoomEditorToolkit::GetRoom() const
{
	return Cast<UGridMapRoomAsset>(GetEditingObject());
}

// ---------------------------------------------------------------------------
// Toolkit 基本信息
// ---------------------------------------------------------------------------

FName FGridMapRoomEditorToolkit::GetToolkitFName() const
{
	return FName(TEXT("GridMapRoomEditor"));
}

FText FGridMapRoomEditorToolkit::GetBaseToolkitName() const
{
	return LOCTEXT("ToolkitName", "房间模板编辑器");
}

FString FGridMapRoomEditorToolkit::GetWorldCentricTabPrefix() const
{
	return LOCTEXT("TabPrefix", "房间模板 ").ToString();
}

FLinearColor FGridMapRoomEditorToolkit::GetWorldCentricTabColorScale() const
{
	return FLinearColor(0.2f, 0.55f, 0.6f, 0.5f);
}

// ---------------------------------------------------------------------------
// Tab 注册
// ---------------------------------------------------------------------------

void FGridMapRoomEditorToolkit::RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	using namespace GridMapRoomToolkitLocal;

	WorkspaceMenuCategory = InTabManager->AddLocalWorkspaceMenuCategory(LOCTEXT("WorkspaceMenu", "房间模板编辑器"));
	FAssetEditorToolkit::RegisterTabSpawners(InTabManager);

	const TSharedRef<FWorkspaceItem> MenuCategory = WorkspaceMenuCategory.ToSharedRef();

	InTabManager->RegisterTabSpawner(PaletteTabId, FOnSpawnTab::CreateSP(this, &FGridMapRoomEditorToolkit::SpawnTab_Palette))
		.SetDisplayName(LOCTEXT("PaletteTab", "工具与调色板"))
		.SetGroup(MenuCategory);

	InTabManager->RegisterTabSpawner(CanvasTabId, FOnSpawnTab::CreateSP(this, &FGridMapRoomEditorToolkit::SpawnTab_Canvas))
		.SetDisplayName(LOCTEXT("CanvasTab", "房间画布"))
		.SetGroup(MenuCategory);

	InTabManager->RegisterTabSpawner(DetailsTabId, FOnSpawnTab::CreateSP(this, &FGridMapRoomEditorToolkit::SpawnTab_Details))
		.SetDisplayName(LOCTEXT("DetailsTab", "资产属性"))
		.SetGroup(MenuCategory);

	InTabManager->RegisterTabSpawner(IssuesTabId, FOnSpawnTab::CreateSP(this, &FGridMapRoomEditorToolkit::SpawnTab_Issues))
		.SetDisplayName(LOCTEXT("IssuesTab", "校验问题"))
		.SetGroup(MenuCategory);
}

void FGridMapRoomEditorToolkit::UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager)
{
	using namespace GridMapRoomToolkitLocal;

	FAssetEditorToolkit::UnregisterTabSpawners(InTabManager);
	InTabManager->UnregisterTabSpawner(PaletteTabId);
	InTabManager->UnregisterTabSpawner(CanvasTabId);
	InTabManager->UnregisterTabSpawner(DetailsTabId);
	InTabManager->UnregisterTabSpawner(IssuesTabId);
}

TSharedRef<SDockTab> FGridMapRoomEditorToolkit::SpawnTab_Canvas(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("CanvasTab", "房间画布"))
		[
			SNew(SBorder)
			.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
			.Padding(0.0f)
			[
				CanvasWidget.ToSharedRef()
			]
		];
}

TSharedRef<SDockTab> FGridMapRoomEditorToolkit::SpawnTab_Palette(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("PaletteTab", "工具与调色板"))
		[
			BuildPalettePanel()
		];
}

TSharedRef<SDockTab> FGridMapRoomEditorToolkit::SpawnTab_Details(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("DetailsTab", "资产属性"))
		[
			SNew(SVerticalBox)

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				DetailsView.ToSharedRef()
			]

			// 选中对象信息：画布上选中的出口 / 标记摘要
			+ SVerticalBox::Slot()
			.AutoHeight()
			[
				SNew(SBorder)
				.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
				.Padding(8.0f)
				[
					SNew(SVerticalBox)

					+ SVerticalBox::Slot()
					.AutoHeight()
					.Padding(0.0f, 0.0f, 0.0f, 4.0f)
					[
						SNew(STextBlock)
						.Font(FAppStyle::GetFontStyle("BoldFont"))
						.Text(LOCTEXT("SelectionHeader", "选中对象"))
					]

					+ SVerticalBox::Slot()
					.AutoHeight()
					[
						SNew(STextBlock)
						.AutoWrapText(true)
						.ColorAndOpacity(FSlateColor(FLinearColor(0.72f, 0.75f, 0.79f)))
						.Text_Lambda([this]
						{
							return CanvasWidget.IsValid() ? CanvasWidget->GetSelectionInfoText() : FText::GetEmpty();
						})
					]
				]
			]
		];
}

TSharedRef<SDockTab> FGridMapRoomEditorToolkit::SpawnTab_Issues(const FSpawnTabArgs& Args)
{
	return SNew(SDockTab)
		.Label(LOCTEXT("IssuesTab", "校验问题"))
		[
			BuildIssuePanel()
		];
}

// ---------------------------------------------------------------------------
// 左侧面板
// ---------------------------------------------------------------------------

TSharedRef<SWidget> FGridMapRoomEditorToolkit::MakeSectionHeader(const FText& Title) const
{
	return SNew(STextBlock)
		.Font(FAppStyle::GetFontStyle("BoldFont"))
		.ColorAndOpacity(FSlateColor(FLinearColor(0.62f, 0.78f, 0.85f)))
		.Text(Title);
}

TSharedRef<SWidget> FGridMapRoomEditorToolkit::MakeToolButton(const EGridMapRoomTool Tool, const FText& Label, const FText& Tooltip)
{
	return SNew(SCheckBox)
		.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
		.ToolTipText(Tooltip)
		.Padding(FMargin(8.0f, 4.0f))
		.IsChecked_Lambda([this, Tool]
		{
			return ActiveTool == Tool ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, Tool](ECheckBoxState)
		{
			ActiveTool = Tool;
		})
		[
			SNew(STextBlock).Text(Label)
		];
}

TSharedRef<SWidget> FGridMapRoomEditorToolkit::BuildToolSection()
{
	return SNew(SWrapBox)
		.UseAllottedSize(true)
		.InnerSlotPadding(FVector2D(4.0, 4.0))

		+ SWrapBox::Slot()[MakeToolButton(EGridMapRoomTool::Brush, LOCTEXT("ToolBrush", "笔刷"),
			LOCTEXT("ToolBrushTip", "左键绘制当前地形，右键擦除为空地。支持 1x1 / 3x3 / 5x5。"))]

		+ SWrapBox::Slot()[MakeToolButton(EGridMapRoomTool::RectFill, LOCTEXT("ToolRect", "矩形"),
			LOCTEXT("ToolRectTip", "按住拖出矩形范围，松开后一次性填充；右键拖动为擦除。"))]

		+ SWrapBox::Slot()[MakeToolButton(EGridMapRoomTool::FloodFill, LOCTEXT("ToolFlood", "区域填充"),
			LOCTEXT("ToolFloodTip", "把点击处连通的同类地形整块替换为当前地形。"))]

		+ SWrapBox::Slot()[MakeToolButton(EGridMapRoomTool::Eyedropper, LOCTEXT("ToolPick", "吸管"),
			LOCTEXT("ToolPickTip", "点击格子，把它的地形设为当前地形。"))]

		+ SWrapBox::Slot()[MakeToolButton(EGridMapRoomTool::Marker, LOCTEXT("ToolMarker", "标记"),
			LOCTEXT("ToolMarkerTip", "左键放置当前标记类型；点住已有标记可拖动；右键删除。"))]

		+ SWrapBox::Slot()[MakeToolButton(EGridMapRoomTool::Exit, LOCTEXT("ToolExit", "出口"),
			LOCTEXT("ToolExitTip", "在房间边缘点击创建出口，按住左键沿边缘拖动位置；Shift+左键拖动改宽度；右键删除。"))];
}

TSharedRef<SWidget> FGridMapRoomEditorToolkit::MakeBrushSizeButton(const EGridMapRoomBrushSize Size, const FText& Label)
{
	return SNew(SCheckBox)
		.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
		.Padding(FMargin(10.0f, 4.0f))
		.IsChecked_Lambda([this, Size]
		{
			return ActiveBrushSize == Size ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, Size](ECheckBoxState)
		{
			ActiveBrushSize = Size;
		})
		[
			SNew(STextBlock).Text(Label)
		];
}

TSharedRef<SWidget> FGridMapRoomEditorToolkit::BuildBrushSizeSection()
{
	return SNew(SWrapBox)
		.UseAllottedSize(true)
		.InnerSlotPadding(FVector2D(4.0, 4.0))

		+ SWrapBox::Slot()[MakeBrushSizeButton(EGridMapRoomBrushSize::One, LOCTEXT("Brush1", "1x1"))]
		+ SWrapBox::Slot()[MakeBrushSizeButton(EGridMapRoomBrushSize::Three, LOCTEXT("Brush3", "3x3"))]
		+ SWrapBox::Slot()[MakeBrushSizeButton(EGridMapRoomBrushSize::Five, LOCTEXT("Brush5", "5x5"))];
}

TSharedRef<SWidget> FGridMapRoomEditorToolkit::MakeTileButton(const EGridTileType Tile)
{
	return SNew(SCheckBox)
		.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
		.Padding(FMargin(6.0f, 4.0f))
		.ToolTipText(GridMapRoomEditor::TileLabel(Tile))
		.IsChecked_Lambda([this, Tile]
		{
			return ActiveTile == Tile ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, Tile](ECheckBoxState)
		{
			ActiveTile = Tile;
		})
		[
			SNew(SHorizontalBox)

			// 色块图例，和画布上的配色保持一致
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(14.0f)
				.HeightOverride(14.0f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("WhiteBrush"))
					.ColorAndOpacity(FSlateColor(GridMapRoomEditor::TileColor(Tile)))
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(GridMapRoomEditor::TileLabel(Tile))
			]
		];
}

TSharedRef<SWidget> FGridMapRoomEditorToolkit::BuildTileSection()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[MakeTileButton(EGridTileType::Empty)]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[MakeTileButton(EGridTileType::Tree)]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[MakeTileButton(EGridTileType::LargeTree)]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[MakeTileButton(EGridTileType::Rock)];
}

TSharedRef<SWidget> FGridMapRoomEditorToolkit::MakeMarkerButton(const EGridMapMarkerType Marker)
{
	return SNew(SCheckBox)
		.Style(FAppStyle::Get(), "ToggleButtonCheckbox")
		.Padding(FMargin(6.0f, 4.0f))
		.ToolTipText(LOCTEXT("MarkerTip", "选中后切换到「标记」工具即可在画布上放置。"))
		.IsChecked_Lambda([this, Marker]
		{
			return ActiveMarker == Marker ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
		})
		.OnCheckStateChanged_Lambda([this, Marker](ECheckBoxState)
		{
			ActiveMarker = Marker;
			// 选了标记类型基本就是想放标记，直接切工具少点一次
			ActiveTool = EGridMapRoomTool::Marker;
		})
		[
			SNew(SHorizontalBox)

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 6.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(14.0f)
				.HeightOverride(14.0f)
				[
					SNew(SImage)
					.Image(FAppStyle::GetBrush("WhiteBrush"))
					.ColorAndOpacity(FSlateColor(GridMapRoomEditor::MarkerColor(Marker)))
				]
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock).Text(GridMapRoomEditor::MarkerLabel(Marker))
			]
		];
}

TSharedRef<SWidget> FGridMapRoomEditorToolkit::BuildMarkerSection()
{
	return SNew(SVerticalBox)

		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[MakeMarkerButton(EGridMapMarkerType::PlayerSpawn)]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[MakeMarkerButton(EGridMapMarkerType::Objective)]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[MakeMarkerButton(EGridMapMarkerType::EnemySpawn)]
		+ SVerticalBox::Slot().AutoHeight().Padding(0.0f, 2.0f)[MakeMarkerButton(EGridMapMarkerType::Decoration)];
}

TSharedRef<SWidget> FGridMapRoomEditorToolkit::BuildHintSection()
{
	return SNew(STextBlock)
		.AutoWrapText(true)
		.ColorAndOpacity(FSlateColor(FLinearColor(0.62f, 0.65f, 0.70f)))
		.Text(LOCTEXT("Hints",
			"左键 = 使用工具\n"
			"右键 = 擦除 / 删除\n"
			"滚轮 = 以光标为中心缩放\n"
			"中键拖动 或 Space+左键 = 平移\n"
			"F = 视图复位并适配房间\n"
			"Ctrl+Z / Ctrl+Y = 撤销 / 重做\n"
			"Shift+左键（出口工具）= 拖动改出口宽度"));
}

TSharedRef<SWidget> FGridMapRoomEditorToolkit::BuildPalettePanel()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(8.0f)
		[
			SNew(SScrollBox)

			+ SScrollBox::Slot().Padding(0.0f, 0.0f, 0.0f, 4.0f)[MakeSectionHeader(LOCTEXT("SectionTools", "工具"))]
			+ SScrollBox::Slot().Padding(0.0f, 0.0f, 0.0f, 10.0f)[BuildToolSection()]

			+ SScrollBox::Slot()[SNew(SSeparator)]

			+ SScrollBox::Slot().Padding(0.0f, 8.0f, 0.0f, 4.0f)[MakeSectionHeader(LOCTEXT("SectionBrush", "笔刷大小"))]
			+ SScrollBox::Slot().Padding(0.0f, 0.0f, 0.0f, 10.0f)[BuildBrushSizeSection()]

			+ SScrollBox::Slot()[SNew(SSeparator)]

			+ SScrollBox::Slot().Padding(0.0f, 8.0f, 0.0f, 4.0f)[MakeSectionHeader(LOCTEXT("SectionTiles", "地形调色板"))]
			+ SScrollBox::Slot().Padding(0.0f, 0.0f, 0.0f, 10.0f)[BuildTileSection()]

			+ SScrollBox::Slot()[SNew(SSeparator)]

			+ SScrollBox::Slot().Padding(0.0f, 8.0f, 0.0f, 4.0f)[MakeSectionHeader(LOCTEXT("SectionMarkers", "标记类型"))]
			+ SScrollBox::Slot().Padding(0.0f, 0.0f, 0.0f, 10.0f)[BuildMarkerSection()]

			+ SScrollBox::Slot()[SNew(SSeparator)]

			+ SScrollBox::Slot().Padding(0.0f, 8.0f, 0.0f, 4.0f)[MakeSectionHeader(LOCTEXT("SectionView", "视图"))]

			+ SScrollBox::Slot()
			.Padding(0.0f, 0.0f, 0.0f, 4.0f)
			[
				SNew(SCheckBox)
				.ToolTipText(LOCTEXT("ReachabilityTip", "从第一个出口（无出口时用第一块空地）做四方向 BFS，把可达的空地铺上绿色。"))
				.IsChecked_Lambda([this]
				{
					return bShowReachability ? ECheckBoxState::Checked : ECheckBoxState::Unchecked;
				})
				.OnCheckStateChanged_Lambda([this](const ECheckBoxState NewState)
				{
					bShowReachability = NewState == ECheckBoxState::Checked;
					if (CanvasWidget.IsValid())
					{
						CanvasWidget->RefreshReachability();
					}
				})
				[
					SNew(STextBlock).Text(LOCTEXT("ShowReachability", "显示可达性叠加"))
				]
			]

			+ SScrollBox::Slot()
			.Padding(0.0f, 0.0f, 0.0f, 10.0f)
			[
				SNew(SButton)
				.HAlign(HAlign_Center)
				.Text(LOCTEXT("ResetView", "视图复位 (F)"))
				.OnClicked_Lambda([this]
				{
					if (CanvasWidget.IsValid())
					{
						CanvasWidget->ResetView();
					}
					return FReply::Handled();
				})
			]

			+ SScrollBox::Slot()[SNew(SSeparator)]

			+ SScrollBox::Slot().Padding(0.0f, 8.0f, 0.0f, 4.0f)[MakeSectionHeader(LOCTEXT("SectionHints", "操作提示"))]
			+ SScrollBox::Slot()[BuildHintSection()]
		];
}

// ---------------------------------------------------------------------------
// 底部校验面板
// ---------------------------------------------------------------------------

TSharedRef<SWidget> FGridMapRoomEditorToolkit::BuildIssuePanel()
{
	return SNew(SBorder)
		.BorderImage(FAppStyle::GetBrush("ToolPanel.GroupBorder"))
		.Padding(6.0f)
		[
			SNew(SVerticalBox)

			// 顶栏：常驻计数 + 完整校验按钮
			+ SVerticalBox::Slot()
			.AutoHeight()
			.Padding(0.0f, 0.0f, 0.0f, 6.0f)
			[
				SNew(SHorizontalBox)

				+ SHorizontalBox::Slot()
				.AutoWidth()
				.VAlign(VAlign_Center)
				.Padding(2.0f, 0.0f, 12.0f, 0.0f)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("BoldFont"))
					.Text(this, &FGridMapRoomEditorToolkit::GetValidationSummaryText)
					.ColorAndOpacity(this, &FGridMapRoomEditorToolkit::GetValidationSummaryColor)
				]

				+ SHorizontalBox::Slot()
				.AutoWidth()
				[
					SNew(SButton)
					.Text(LOCTEXT("RunValidation", "执行完整校验"))
					.ToolTipText(LOCTEXT("RunValidationTip", "调用 FGridMapRoomValidator::Validate 重新检查全部规则。"))
					.OnClicked_Lambda([this]
					{
						RunValidation();
						return FReply::Handled();
					})
				]

				+ SHorizontalBox::Slot()
				.FillWidth(1.0f)
				.VAlign(VAlign_Center)
				.HAlign(HAlign_Right)
				.Padding(8.0f, 0.0f, 2.0f, 0.0f)
				[
					SNew(STextBlock)
					.ColorAndOpacity(FSlateColor(FLinearColor(0.6f, 0.63f, 0.68f)))
					.Text(LOCTEXT("IssueHint", "单击带坐标的问题可定位到对应格子"))
				]
			]

			+ SVerticalBox::Slot()
			.FillHeight(1.0f)
			[
				SAssignNew(IssueListView, SListView<FGridMapIssuePtr>)
				.ListItemsSource(&IssueItems)
				.SelectionMode(ESelectionMode::Single)
				.OnGenerateRow(this, &FGridMapRoomEditorToolkit::OnGenerateIssueRow)
				.OnMouseButtonClick(this, &FGridMapRoomEditorToolkit::OnIssueClicked)
			]
		];
}

TSharedRef<ITableRow> FGridMapRoomEditorToolkit::OnGenerateIssueRow(FGridMapIssuePtr Item, const TSharedRef<STableViewBase>& OwnerTable)
{
	const bool bHasCoord = Item.IsValid() && Item->GridCoord.X >= 0 && Item->GridCoord.Y >= 0;
	const FText CoordText = bHasCoord
		? FText::FromString(FString::Printf(TEXT("(%d, %d)"), Item->GridCoord.X, Item->GridCoord.Y))
		: LOCTEXT("NoCoord", "—");

	return SNew(STableRow<FGridMapIssuePtr>, OwnerTable)
		.Padding(FMargin(4.0f, 3.0f))
		[
			SNew(SHorizontalBox)

			// 等级
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(38.0f)
				[
					SNew(STextBlock)
					.Font(FAppStyle::GetFontStyle("BoldFont"))
					.ColorAndOpacity(FSlateColor(GridMapRoomToolkitLocal::SeverityColor(Item->Severity)))
					.Text(GridMapRoomToolkitLocal::SeverityLabel(Item->Severity))
				]
			]

			// 坐标
			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(0.0f, 0.0f, 10.0f, 0.0f)
			[
				SNew(SBox)
				.WidthOverride(72.0f)
				[
					SNew(STextBlock)
					.ColorAndOpacity(FSlateColor(bHasCoord ? FLinearColor(0.55f, 0.80f, 0.90f) : FLinearColor(0.45f, 0.47f, 0.50f)))
					.Text(CoordText)
				]
			]

			// 描述 + 规则代码
			+ SHorizontalBox::Slot()
			.FillWidth(1.0f)
			.VAlign(VAlign_Center)
			[
				SNew(STextBlock)
				.Text(FText::FromString(Item->Message))
			]

			+ SHorizontalBox::Slot()
			.AutoWidth()
			.VAlign(VAlign_Center)
			.Padding(10.0f, 0.0f, 4.0f, 0.0f)
			[
				SNew(STextBlock)
				.ColorAndOpacity(FSlateColor(FLinearColor(0.45f, 0.47f, 0.50f)))
				.Text(FText::FromName(Item->Code))
			]
		];
}

void FGridMapRoomEditorToolkit::OnIssueClicked(FGridMapIssuePtr Item)
{
	if (Item.IsValid() && CanvasWidget.IsValid() && Item->GridCoord.X >= 0 && Item->GridCoord.Y >= 0)
	{
		CanvasWidget->FocusOnCoord(Item->GridCoord);
	}
}

// ---------------------------------------------------------------------------
// 校验
// ---------------------------------------------------------------------------

void FGridMapRoomEditorToolkit::RunValidation()
{
	IssueItems.Reset();
	ErrorCount = 0;
	WarningCount = 0;

	if (const UGridMapRoomAsset* RoomPtr = GetRoom())
	{
		for (const FGridMapValidationIssue& Issue : FGridMapRoomValidator::Validate(*RoomPtr))
		{
			if (Issue.Severity == EGridMapValidationSeverity::Error)
			{
				++ErrorCount;
			}
			else
			{
				++WarningCount;
			}
			IssueItems.Add(MakeShared<FGridMapValidationIssue>(Issue));
		}
	}

	if (IssueListView.IsValid())
	{
		IssueListView->RequestListRefresh();
	}
}

FText FGridMapRoomEditorToolkit::GetValidationSummaryText() const
{
	if (ErrorCount == 0 && WarningCount == 0)
	{
		return LOCTEXT("ValidationClean", "校验通过：0 错误 / 0 警告");
	}
	return FText::FromString(FString::Printf(TEXT("%d 错误 / %d 警告"), ErrorCount, WarningCount));
}

FSlateColor FGridMapRoomEditorToolkit::GetValidationSummaryColor() const
{
	if (ErrorCount > 0)
	{
		return FSlateColor(FLinearColor(0.925f, 0.325f, 0.271f));
	}
	if (WarningCount > 0)
	{
		return FSlateColor(FLinearColor(0.976f, 0.729f, 0.196f));
	}
	return FSlateColor(FLinearColor(0.325f, 0.812f, 0.478f));
}

// ---------------------------------------------------------------------------
// 数据同步
// ---------------------------------------------------------------------------

void FGridMapRoomEditorToolkit::HandleRoomModified()
{
	RunValidation();
}

void FGridMapRoomEditorToolkit::HandleTilePicked(const EGridTileType Tile)
{
	ActiveTile = Tile;
}

void FGridMapRoomEditorToolkit::HandleCanvasSelectionChanged()
{
	// 选中信息是通过 Text_Lambda 拉取的，这里无需额外刷新
}

void FGridMapRoomEditorToolkit::ConformRoomToSize()
{
	UGridMapRoomAsset* RoomPtr = GetRoom();
	if (!RoomPtr)
	{
		return;
	}

	using namespace GridMapRoomToolkitLocal;
	const FIntPoint RequestedSize = RoomPtr->Size;
	const int64 DesiredCount64 = static_cast<int64>(RequestedSize.X) * RequestedSize.Y;
	if (RequestedSize.X < 1 || RequestedSize.Y < 1
		|| RequestedSize.X > MaxRoomDimension || RequestedSize.Y > MaxRoomDimension
		|| DesiredCount64 > MAX_int32)
	{
		RoomPtr->Size = LastKnownSize;
		FNotificationInfo Info(FText::Format(
			LOCTEXT("InvalidRoomSize", "房间尺寸必须在 1 到 {0} 之间，已恢复原尺寸。"),
			FText::AsNumber(MaxRoomDimension)));
		Info.ExpireDuration = 5.0f;
		Info.bFireAndForget = true;
		FSlateNotificationManager::Get().AddNotification(Info);
		if (DetailsView.IsValid())
		{
			DetailsView->ForceRefresh();
		}
		return;
	}

	const int32 Width = RequestedSize.X;
	const int32 Height = RequestedSize.Y;
	const TArray<EGridTileType> OldTiles = RoomPtr->Tiles;
	TArray<EGridTileType> NewTiles;
	NewTiles.Init(EGridTileType::Empty, static_cast<int32>(DesiredCount64));
	const int32 CopyWidth = FMath::Min(LastKnownSize.X, Width);
	const int32 CopyHeight = FMath::Min(LastKnownSize.Y, Height);
	for (int32 Y = 0; Y < CopyHeight; ++Y)
	{
		for (int32 X = 0; X < CopyWidth; ++X)
		{
			const int64 OldIndex = static_cast<int64>(Y) * LastKnownSize.X + X;
			if (OldIndex >= 0 && OldIndex < OldTiles.Num())
			{
				NewTiles[Y * Width + X] = OldTiles[static_cast<int32>(OldIndex)];
			}
		}
	}
	RoomPtr->Tiles = MoveTemp(NewTiles);

	// 按出口方向重新贴边，并限制沿边起点和宽度。
	for (FGridMapRoomExit& Exit : RoomPtr->Exits)
	{
		const bool bHorizontalEdge = Exit.Direction == EGridMapExitDirection::North || Exit.Direction == EGridMapExitDirection::South;
		const int32 AxisMax = bHorizontalEdge ? RoomPtr->Size.X : RoomPtr->Size.Y;
		Exit.Width = FMath::Clamp(Exit.Width, 1, AxisMax);
		const int32 AxisStart = FMath::Clamp(bHorizontalEdge ? Exit.Start.X : Exit.Start.Y, 0, AxisMax - Exit.Width);
		if (bHorizontalEdge)
		{
			Exit.Start.X = AxisStart;
			Exit.Start.Y = Exit.Direction == EGridMapExitDirection::North ? Height - 1 : 0;
		}
		else
		{
			Exit.Start.X = Exit.Direction == EGridMapExitDirection::East ? Width - 1 : 0;
			Exit.Start.Y = AxisStart;
		}
	}
	RoomPtr->Markers.RemoveAll([RoomPtr](const FGridMapRoomMarker& Marker)
	{
		return !RoomPtr->IsInBounds(Marker.GridCoord);
	});
}

void FGridMapRoomEditorToolkit::HandlePropertyChanged(const FPropertyChangedEvent& PropertyChangedEvent)
{
	UGridMapRoomAsset* RoomPtr = GetRoom();
	if (!RoomPtr)
	{
		return;
	}

	const FName PropertyName = PropertyChangedEvent.GetMemberPropertyName();

	// 改尺寸：让 Tiles 与 Size 保持一致，并清掉越界的出口 / 标记
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UGridMapRoomAsset, Size) && RoomPtr->Size != LastKnownSize)
	{
		const FIntPoint PreviousSize = LastKnownSize;
		ConformRoomToSize();
		LastKnownSize = RoomPtr->Size;
		if (RoomPtr->Size != PreviousSize)
		{
			RoomPtr->MarkPackageDirty();
		}
	}

	// Draft → Ready：必须先没有 Error，否则打回 Draft
	if (PropertyName == GET_MEMBER_NAME_CHECKED(UGridMapRoomAsset, Status)
		&& RoomPtr->Status == EGridMapRoomStatus::Ready
		&& LastKnownStatus != EGridMapRoomStatus::Ready)
	{
		TArray<FGridMapValidationIssue> Issues = FGridMapRoomValidator::Validate(*RoomPtr);
		// Ready 状态下 Validator 会额外追加一条 ReadyAssetInvalid，这里只看真实错误
		const int32 RealErrorCount = Issues.FilterByPredicate([](const FGridMapValidationIssue& Issue)
		{
			return Issue.Severity == EGridMapValidationSeverity::Error && Issue.Code != FName(TEXT("ReadyAssetInvalid"));
		}).Num();

		if (RealErrorCount > 0)
		{
			RoomPtr->Status = EGridMapRoomStatus::Draft;

			FNotificationInfo Info(FText::FromString(
				FString::Printf(TEXT("房间仍有 %d 个错误，无法标记为 Ready，已恢复为 Draft。"), RealErrorCount)));
			Info.ExpireDuration = 5.0f;
			Info.bFireAndForget = true;
			FSlateNotificationManager::Get().AddNotification(Info);

			if (DetailsView.IsValid())
			{
				DetailsView->ForceRefresh();
			}
		}
	}

	LastKnownStatus = RoomPtr->Status;
	LastKnownSize = RoomPtr->Size;

	if (CanvasWidget.IsValid())
	{
		CanvasWidget->RefreshFromAsset();
	}
	RunValidation();
}

// ---------------------------------------------------------------------------
// 撤销 / 重做
// ---------------------------------------------------------------------------

void FGridMapRoomEditorToolkit::PostUndo(bool bSuccess)
{
	if (const UGridMapRoomAsset* RoomPtr = GetRoom())
	{
		LastKnownStatus = RoomPtr->Status;
		LastKnownSize = RoomPtr->Size;
	}
	if (CanvasWidget.IsValid())
	{
		CanvasWidget->RefreshFromAsset();
	}
	if (DetailsView.IsValid())
	{
		DetailsView->ForceRefresh();
	}
	RunValidation();
}

void FGridMapRoomEditorToolkit::PostRedo(bool bSuccess)
{
	PostUndo(bSuccess);
}

#undef LOCTEXT_NAMESPACE
