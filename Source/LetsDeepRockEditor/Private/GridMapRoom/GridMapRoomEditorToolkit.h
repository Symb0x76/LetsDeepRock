// 网格房间模板编辑器 - 资产编辑器 Toolkit

#pragma once

#include "CoreMinimal.h"
#include "EditorUndoClient.h"
#include "GridMap/GridMapRoomValidator.h"
#include "GridMapRoomEditorTypes.h"
#include "Toolkits/AssetEditorToolkit.h"

class IDetailsView;
class SGridMapRoomCanvas;
class UGridMapRoomAsset;
template <typename ItemType> class SListView;

/** 校验问题列表项。 */
using FGridMapIssuePtr = TSharedPtr<FGridMapValidationIssue>;

/**
 * 房间模板专用资产编辑器：左侧工具面板 + 中间自绘画布 + 右侧 Details + 底部问题列表。
 */
class FGridMapRoomEditorToolkit : public FAssetEditorToolkit, public FEditorUndoClient
{
public:
	void InitEditor(EToolkitMode::Type Mode, const TSharedPtr<IToolkitHost>& InitToolkitHost, UGridMapRoomAsset* InRoom);

	virtual ~FGridMapRoomEditorToolkit() override;

	// FAssetEditorToolkit
	virtual void RegisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual void UnregisterTabSpawners(const TSharedRef<FTabManager>& InTabManager) override;
	virtual FName GetToolkitFName() const override;
	virtual FText GetBaseToolkitName() const override;
	virtual FString GetWorldCentricTabPrefix() const override;
	virtual FLinearColor GetWorldCentricTabColorScale() const override;

	// FEditorUndoClient
	virtual void PostUndo(bool bSuccess) override;
	virtual void PostRedo(bool bSuccess) override;

private:
	// --- Tab ---
	TSharedRef<SDockTab> SpawnTab_Canvas(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnTab_Palette(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnTab_Details(const FSpawnTabArgs& Args);
	TSharedRef<SDockTab> SpawnTab_Issues(const FSpawnTabArgs& Args);

	// --- 面板构建 ---
	TSharedRef<SWidget> BuildPalettePanel();
	TSharedRef<SWidget> BuildToolSection();
	TSharedRef<SWidget> BuildBrushSizeSection();
	TSharedRef<SWidget> BuildTileSection();
	TSharedRef<SWidget> BuildMarkerSection();
	TSharedRef<SWidget> BuildHintSection();
	TSharedRef<SWidget> BuildIssuePanel();
	TSharedRef<SWidget> MakeSectionHeader(const FText& Title) const;
	TSharedRef<SWidget> MakeToolButton(EGridMapRoomTool Tool, const FText& Label, const FText& Tooltip);
	TSharedRef<SWidget> MakeBrushSizeButton(EGridMapRoomBrushSize Size, const FText& Label);
	TSharedRef<SWidget> MakeTileButton(EGridTileType Tile);
	TSharedRef<SWidget> MakeMarkerButton(EGridMapMarkerType Marker);

	// --- 校验 ---
	void RunValidation();
	FText GetValidationSummaryText() const;
	FSlateColor GetValidationSummaryColor() const;
	TSharedRef<ITableRow> OnGenerateIssueRow(FGridMapIssuePtr Item, const TSharedRef<STableViewBase>& OwnerTable);
	void OnIssueClicked(FGridMapIssuePtr Item);

	// --- 数据同步 ---
	void HandleRoomModified();
	void HandleTilePicked(EGridTileType Tile);
	void HandleCanvasSelectionChanged();
	void HandlePropertyChanged(const FPropertyChangedEvent& PropertyChangedEvent);

	/** 保证 Tiles 长度与 Size 匹配，并裁掉越界的出口 / 标记。 */
	void ConformRoomToSize();

	UGridMapRoomAsset* GetRoom() const;

	TSharedPtr<SGridMapRoomCanvas> CanvasWidget;
	TSharedPtr<IDetailsView> DetailsView;
	TSharedPtr<SListView<FGridMapIssuePtr>> IssueListView;

	TArray<FGridMapIssuePtr> IssueItems;
	int32 ErrorCount = 0;
	int32 WarningCount = 0;

	// 调色板当前状态
	EGridMapRoomTool ActiveTool = EGridMapRoomTool::Brush;
	EGridTileType ActiveTile = EGridTileType::Rock;
	EGridMapMarkerType ActiveMarker = EGridMapMarkerType::EnemySpawn;
	EGridMapRoomBrushSize ActiveBrushSize = EGridMapRoomBrushSize::One;
	bool bShowReachability = false;

	/** Details 中改 Status 前的旧值，Ready 校验不通过时用于回滚。 */
	EGridMapRoomStatus LastKnownStatus = EGridMapRoomStatus::Draft;
	/** Details 中改 Size 前的旧值，用于判断是否需要重建 Tiles。 */
	FIntPoint LastKnownSize = FIntPoint::ZeroValue;
};
