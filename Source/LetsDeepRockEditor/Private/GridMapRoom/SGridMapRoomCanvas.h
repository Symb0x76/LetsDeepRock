// 网格房间模板编辑器 - 自绘画布（整块 Slate 绘制，不为每格创建 Widget）

#pragma once

#include "CoreMinimal.h"
#include "GridMapRoomEditorTypes.h"
#include "Widgets/SLeafWidget.h"

class FScopedTransaction;
class UGridMapRoomAsset;
struct FGridMapRoomExit;

/**
 * 单个 Widget 负责整张房间网格的绘制与交互。
 * 所有格子在 OnPaint 中以 Draw Element 批量输出，屏幕外区域直接裁剪。
 */
class SGridMapRoomCanvas : public SLeafWidget
{
public:
	DECLARE_DELEGATE(FOnRoomModified);
	DECLARE_DELEGATE_OneParam(FOnTilePicked, EGridTileType);
	DECLARE_DELEGATE(FOnCanvasSelectionChanged);

	SLATE_BEGIN_ARGS(SGridMapRoomCanvas) {}
		SLATE_ARGUMENT(UGridMapRoomAsset*, Room)
		SLATE_ATTRIBUTE(EGridMapRoomTool, ActiveTool)
		SLATE_ATTRIBUTE(EGridTileType, PaintTile)
		SLATE_ATTRIBUTE(EGridMapMarkerType, MarkerType)
		SLATE_ATTRIBUTE(EGridMapRoomBrushSize, BrushSize)
		SLATE_ATTRIBUTE(bool, ShowReachability)
		/** 笔划结束或数据被修改时触发，用于刷新校验。 */
		SLATE_EVENT(FOnRoomModified, OnRoomModified)
		/** 吸管取到地形时回传给调色板。 */
		SLATE_EVENT(FOnTilePicked, OnTilePicked)
		/** 选中的标记 / 出口发生变化。 */
		SLATE_EVENT(FOnCanvasSelectionChanged, OnCanvasSelectionChanged)
	SLATE_END_ARGS()

	void Construct(const FArguments& InArgs);

	// SWidget
	virtual int32 OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
		FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const override;
	virtual FVector2D ComputeDesiredSize(float) const override;
	virtual bool SupportsKeyboardFocus() const override { return true; }
	virtual FReply OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual FReply OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual FReply OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent) override;
	virtual void OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent) override;
	virtual void OnMouseLeave(const FPointerEvent& MouseEvent) override;

	/** 撤销 / 重做或尺寸变化后调用，丢弃悬空选中并重建可达性缓存。 */
	void RefreshFromAsset();

	/** 重新计算可达性叠加缓存。 */
	void RefreshReachability();

	/** 把视图居中到指定格子并高亮，用于问题列表定位。 */
	void FocusOnCoord(FIntPoint Coord);

	/** 视图复位到居中且合适的缩放。 */
	void ResetView();

	/** 右侧面板展示的当前选中对象描述。 */
	FText GetSelectionInfoText() const;

private:
	enum class EDragMode : uint8
	{
		None,
		Paint,		// 笔刷 / 标记连续绘制
		Rect,		// 矩形填充框选
		Pan,		// 平移
		ExitMove,	// 出口沿边缘移动
		ExitResize,	// 出口宽度调整
		MarkerMove	// 拖动已有标记
	};

	// --- 坐标换算 ---
	float GetCellSize() const;
	FVector2f CellToLocal(FIntPoint Coord) const;
	FIntPoint LocalToCell(FVector2f LocalPos) const;
	bool IsValidCoord(FIntPoint Coord) const;

	// --- 事务 ---
	void BeginStroke(const FText& Description);
	void EndStroke();

	// --- 工具行为 ---
	void ApplyBrush(FIntPoint Coord, EGridTileType Tile);
	void ApplyBrushLine(FIntPoint From, FIntPoint To, EGridTileType Tile);
	void ApplyRect(FIntPoint A, FIntPoint B, EGridTileType Tile);
	void ApplyFloodFill(FIntPoint Coord, EGridTileType Tile);
	void ApplyMarker(FIntPoint Coord, bool bErase);
	void HandleExitPress(FIntPoint Coord, bool bErase, bool bResizeModifier);
	void DragExitTo(FIntPoint Coord);

	// --- 命中测试 ---
	int32 FindMarkerAt(FIntPoint Coord) const;
	int32 FindExitAt(FIntPoint Coord) const;
	bool IsCoordOnEdge(FIntPoint Coord, EGridMapExitDirection& OutDirection) const;
	static bool ExitCoversCoord(const FGridMapRoomExit& Exit, FIntPoint Coord);

	void ClearSelection();
	void NotifyModified();

	TWeakObjectPtr<UGridMapRoomAsset> Room;

	TAttribute<EGridMapRoomTool> ActiveTool;
	TAttribute<EGridTileType> PaintTile;
	TAttribute<EGridMapMarkerType> MarkerType;
	TAttribute<EGridMapRoomBrushSize> BrushSize;
	TAttribute<bool> bShowReachability;

	FOnRoomModified OnRoomModified;
	FOnTilePicked OnTilePicked;
	FOnCanvasSelectionChanged OnCanvasSelectionChanged;

	/** 100% 缩放下每格的像素边长。 */
	static constexpr float BaseCellSize = 24.0f;

	float ZoomLevel = 1.0f;
	FVector2f PanOffset = FVector2f(24.0f, 24.0f);

	EDragMode DragMode = EDragMode::None;
	bool bEraseDrag = false;
	bool bSpaceDown = false;
	bool bStrokeModified = false;

	FIntPoint HoverCoord = FIntPoint(INDEX_NONE, INDEX_NONE);
	FIntPoint DragStartCoord = FIntPoint(INDEX_NONE, INDEX_NONE);
	FIntPoint LastPaintedCoord = FIntPoint(INDEX_NONE, INDEX_NONE);
	FIntPoint HighlightCoord = FIntPoint(INDEX_NONE, INDEX_NONE);

	int32 SelectedMarkerIndex = INDEX_NONE;
	int32 SelectedExitIndex = INDEX_NONE;

	/** 可达性叠加缓存。 */
	TSet<FIntPoint> ReachableCells;

	TUniquePtr<FScopedTransaction> ActiveTransaction;
};
