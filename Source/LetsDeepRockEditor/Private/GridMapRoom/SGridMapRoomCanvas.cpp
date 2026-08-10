// 网格房间模板编辑器 - 自绘画布

#include "SGridMapRoomCanvas.h"

#include "Brushes/SlateColorBrush.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "GridMap/GridMapRoomAsset.h"
#include "Rendering/DrawElements.h"
#include "Rendering/SlateRenderer.h"
#include "ScopedTransaction.h"
#include "Styling/CoreStyle.h"

#define LOCTEXT_NAMESPACE "GridMapRoomCanvas"

namespace GridMapRoomCanvasLocal
{
	static const FSlateColorBrush WhiteBrush(FLinearColor::White);

	/** 网格四邻步长。 */
	static const FIntPoint NeighborSteps[] =
	{
		FIntPoint(1, 0), FIntPoint(-1, 0), FIntPoint(0, 1), FIntPoint(0, -1)
	};

	FIntPoint ExitCoordAt(const FGridMapRoomExit& Exit, const int32 Offset)
	{
		return Exit.Start + GridMapRoomEditor::ExitSpanStep(Exit.Direction) * Offset;
	}

	/** 生成不与现有出口重复的 ID。 */
	FName MakeUniqueExitId(const UGridMapRoomAsset& InRoom)
	{
		for (int32 Suffix = 1; Suffix < 1000; ++Suffix)
		{
			const FName Candidate(*FString::Printf(TEXT("Exit_%d"), Suffix));
			const bool bTaken = InRoom.Exits.ContainsByPredicate(
				[Candidate](const FGridMapRoomExit& Exit) { return Exit.Id == Candidate; });
			if (!bTaken)
			{
				return Candidate;
			}
		}
		return FName(TEXT("Exit_New"));
	}

	/** 把一个格子的矩形轮廓写入线段数组。 */
	void BuildCellOutline(TArray<FVector2f>& OutPoints, const FVector2f TopLeft, const float Cell)
	{
		OutPoints.Reset();
		OutPoints.Add(TopLeft);
		OutPoints.Add(TopLeft + FVector2f(Cell, 0.0f));
		OutPoints.Add(TopLeft + FVector2f(Cell, Cell));
		OutPoints.Add(TopLeft + FVector2f(0.0f, Cell));
		OutPoints.Add(TopLeft);
	}
}

void SGridMapRoomCanvas::Construct(const FArguments& InArgs)
{
	Room = InArgs._Room;
	ActiveTool = InArgs._ActiveTool;
	PaintTile = InArgs._PaintTile;
	MarkerType = InArgs._MarkerType;
	BrushSize = InArgs._BrushSize;
	bShowReachability = InArgs._ShowReachability;
	OnRoomModified = InArgs._OnRoomModified;
	OnTilePicked = InArgs._OnTilePicked;
	OnCanvasSelectionChanged = InArgs._OnCanvasSelectionChanged;

	SetClipping(EWidgetClipping::ClipToBounds);
	RefreshReachability();
}

// ---------------------------------------------------------------------------
// 坐标换算
// ---------------------------------------------------------------------------

float SGridMapRoomCanvas::GetCellSize() const
{
	return BaseCellSize * ZoomLevel;
}

FVector2f SGridMapRoomCanvas::CellToLocal(const FIntPoint Coord) const
{
	const float Cell = GetCellSize();
	return PanOffset + FVector2f(Coord.X * Cell, Coord.Y * Cell);
}

FIntPoint SGridMapRoomCanvas::LocalToCell(const FVector2f LocalPos) const
{
	const float Cell = GetCellSize();
	if (Cell <= KINDA_SMALL_NUMBER)
	{
		return FIntPoint(INDEX_NONE, INDEX_NONE);
	}
	const FVector2f Relative = LocalPos - PanOffset;
	return FIntPoint(FMath::FloorToInt32(Relative.X / Cell), FMath::FloorToInt32(Relative.Y / Cell));
}

bool SGridMapRoomCanvas::IsValidCoord(const FIntPoint Coord) const
{
	const UGridMapRoomAsset* RoomPtr = Room.Get();
	return RoomPtr && RoomPtr->IsInBounds(Coord);
}

FVector2D SGridMapRoomCanvas::ComputeDesiredSize(float) const
{
	return FVector2D(640.0, 480.0);
}

// ---------------------------------------------------------------------------
// 绘制
// ---------------------------------------------------------------------------

int32 SGridMapRoomCanvas::OnPaint(const FPaintArgs& Args, const FGeometry& AllottedGeometry, const FSlateRect& MyCullingRect,
	FSlateWindowElementList& OutDrawElements, int32 LayerId, const FWidgetStyle& InWidgetStyle, bool bParentEnabled) const
{
	using namespace GridMapRoomCanvasLocal;

	const UGridMapRoomAsset* RoomPtr = Room.Get();
	const FVector2f ViewSize = AllottedGeometry.GetLocalSize();

	// 背景
	FSlateDrawElement::MakeBox(OutDrawElements, LayerId,
		AllottedGeometry.ToPaintGeometry(), &WhiteBrush, ESlateDrawEffect::None, FLinearColor(0.043f, 0.047f, 0.055f));

	const FSlateFontInfo HintFont = FCoreStyle::GetDefaultFontStyle("Regular", 10);

	if (!RoomPtr || RoomPtr->Size.X <= 0 || RoomPtr->Size.Y <= 0)
	{
		FSlateDrawElement::MakeText(OutDrawElements, LayerId + 1,
			AllottedGeometry.ToPaintGeometry(FVector2f(420.0f, 20.0f), FSlateLayoutTransform(FVector2f(16.0f, 16.0f))),
			LOCTEXT("NoRoomData", "房间尺寸无效，请在右侧 Details 面板设置 Size。").ToString(),
			HintFont, ESlateDrawEffect::None, FLinearColor(0.7f, 0.7f, 0.7f));
		return LayerId + 2;
	}

	const float Cell = GetCellSize();
	const int32 BackgroundLayer = LayerId + 1;
	const int32 OverlayLayer = LayerId + 2;
	const int32 GridLayer = LayerId + 3;
	const int32 ObjectLayer = LayerId + 4;
	const int32 CursorLayer = LayerId + 5;
	const int32 TextLayer = LayerId + 6;

	// 只绘制视口内可见的格子范围，房间再大也保持恒定绘制量
	const FIntPoint MinVisible = LocalToCell(FVector2f::ZeroVector);
	const FIntPoint MaxVisible = LocalToCell(ViewSize);
	const int32 StartX = FMath::Clamp(MinVisible.X, 0, RoomPtr->Size.X - 1);
	const int32 StartY = FMath::Clamp(MinVisible.Y, 0, RoomPtr->Size.Y - 1);
	const int32 EndX = FMath::Clamp(MaxVisible.X, 0, RoomPtr->Size.X - 1);
	const int32 EndY = FMath::Clamp(MaxVisible.Y, 0, RoomPtr->Size.Y - 1);

	const bool bTilesValid = RoomPtr->Tiles.Num() == RoomPtr->Size.X * RoomPtr->Size.Y;
	const bool bReachabilityOn = bShowReachability.Get(false);

	// 地形填充：把水平相邻的同色格子合并成一条长条，显著减少 Draw Element 数量
	if (bTilesValid)
	{
		for (int32 Y = StartY; Y <= EndY; ++Y)
		{
			int32 RunStart = StartX;
			EGridTileType RunTile = RoomPtr->GetTile(FIntPoint(StartX, Y));

			for (int32 X = StartX; X <= EndX + 1; ++X)
			{
				const bool bEnd = X > EndX;
				const EGridTileType Current = bEnd ? EGridTileType::Rock : RoomPtr->GetTile(FIntPoint(X, Y));

				if (bEnd || Current != RunTile)
				{
					const FVector2f TopLeft = CellToLocal(FIntPoint(RunStart, Y));
					const FVector2f RunSize((X - RunStart) * Cell, Cell);
					FSlateDrawElement::MakeBox(OutDrawElements, BackgroundLayer,
						AllottedGeometry.ToPaintGeometry(RunSize, FSlateLayoutTransform(TopLeft)),
						&WhiteBrush, ESlateDrawEffect::None, GridMapRoomEditor::TileColor(RunTile));

					RunStart = X;
					RunTile = Current;
				}
			}
		}
	}

	// 可达性叠加：可通行且可达的格子铺一层浅绿
	if (bReachabilityOn && ReachableCells.Num() > 0)
	{
		const FLinearColor ReachTint(0.216f, 0.796f, 0.463f, 0.30f);
		for (int32 Y = StartY; Y <= EndY; ++Y)
		{
			int32 RunStart = INDEX_NONE;
			for (int32 X = StartX; X <= EndX + 1; ++X)
			{
				const bool bReach = X <= EndX && ReachableCells.Contains(FIntPoint(X, Y));
				if (bReach && RunStart == INDEX_NONE)
				{
					RunStart = X;
				}
				else if (!bReach && RunStart != INDEX_NONE)
				{
					const FVector2f TopLeft = CellToLocal(FIntPoint(RunStart, Y));
					FSlateDrawElement::MakeBox(OutDrawElements, OverlayLayer,
						AllottedGeometry.ToPaintGeometry(FVector2f((X - RunStart) * Cell, Cell), FSlateLayoutTransform(TopLeft)),
						&WhiteBrush, ESlateDrawEffect::None, ReachTint);
					RunStart = INDEX_NONE;
				}
			}
		}
	}

	TArray<FVector2f> LinePoints;

	// 网格线：缩得太小就不画，避免糊成一片
	if (Cell >= 6.0f)
	{
		const FLinearColor LineColor(1.0f, 1.0f, 1.0f, 0.07f);
		const FVector2f Origin = CellToLocal(FIntPoint(StartX, StartY));
		const FVector2f Extent = CellToLocal(FIntPoint(EndX + 1, EndY + 1));

		LinePoints.SetNumUninitialized(2);
		for (int32 X = StartX; X <= EndX + 1; ++X)
		{
			const float Px = CellToLocal(FIntPoint(X, 0)).X;
			LinePoints[0] = FVector2f(Px, Origin.Y);
			LinePoints[1] = FVector2f(Px, Extent.Y);
			FSlateDrawElement::MakeLines(OutDrawElements, GridLayer, AllottedGeometry.ToPaintGeometry(),
				LinePoints, ESlateDrawEffect::None, LineColor, false, 1.0f);
		}
		for (int32 Y = StartY; Y <= EndY + 1; ++Y)
		{
			const float Py = CellToLocal(FIntPoint(0, Y)).Y;
			LinePoints[0] = FVector2f(Origin.X, Py);
			LinePoints[1] = FVector2f(Extent.X, Py);
			FSlateDrawElement::MakeLines(OutDrawElements, GridLayer, AllottedGeometry.ToPaintGeometry(),
				LinePoints, ESlateDrawEffect::None, LineColor, false, 1.0f);
		}
	}

	// 房间外框
	{
		const FVector2f TopLeft = CellToLocal(FIntPoint(0, 0));
		const FVector2f BottomRight = CellToLocal(RoomPtr->Size);
		LinePoints.Reset();
		LinePoints.Add(TopLeft);
		LinePoints.Add(FVector2f(BottomRight.X, TopLeft.Y));
		LinePoints.Add(BottomRight);
		LinePoints.Add(FVector2f(TopLeft.X, BottomRight.Y));
		LinePoints.Add(TopLeft);
		FSlateDrawElement::MakeLines(OutDrawElements, GridLayer, AllottedGeometry.ToPaintGeometry(),
			LinePoints, ESlateDrawEffect::None, FLinearColor(0.55f, 0.58f, 0.65f), true, 2.0f);
	}

	const FSlateFontInfo GlyphFont = FCoreStyle::GetDefaultFontStyle("Bold", FMath::Clamp(Cell * 0.5f, 6.0f, 16.0f));
	const TSharedRef<FSlateFontMeasure> FontMeasure = FSlateApplication::Get().GetRenderer()->GetFontMeasureService();

	// 出口：贴边高亮 + 选中描边
	for (int32 ExitIndex = 0; ExitIndex < RoomPtr->Exits.Num(); ++ExitIndex)
	{
		const FGridMapRoomExit& Exit = RoomPtr->Exits[ExitIndex];
		const bool bSelected = ExitIndex == SelectedExitIndex;
		const FLinearColor Color = GridMapRoomEditor::ExitColor();

		for (int32 Offset = 0; Offset < FMath::Max(Exit.Width, 1); ++Offset)
		{
			const FIntPoint Coord = ExitCoordAt(Exit, Offset);
			if (!RoomPtr->IsInBounds(Coord))
			{
				continue;
			}
			const FVector2f TopLeft = CellToLocal(Coord);
			FSlateDrawElement::MakeBox(OutDrawElements, ObjectLayer,
				AllottedGeometry.ToPaintGeometry(FVector2f(Cell, Cell), FSlateLayoutTransform(TopLeft)),
				&WhiteBrush, ESlateDrawEffect::None, Color.CopyWithNewOpacity(bSelected ? 0.9f : 0.55f));

			if (bSelected)
			{
				BuildCellOutline(LinePoints, TopLeft, Cell);
				FSlateDrawElement::MakeLines(OutDrawElements, CursorLayer, AllottedGeometry.ToPaintGeometry(),
					LinePoints, ESlateDrawEffect::None, FLinearColor::White, true, 2.0f);
			}
		}
	}

	// 语义标记：色块 + 首字母
	for (int32 MarkerIndex = 0; MarkerIndex < RoomPtr->Markers.Num(); ++MarkerIndex)
	{
		const FGridMapRoomMarker& Marker = RoomPtr->Markers[MarkerIndex];
		if (!RoomPtr->IsInBounds(Marker.GridCoord))
		{
			continue;
		}
		const FVector2f TopLeft = CellToLocal(Marker.GridCoord);
		const float Inset = Cell * 0.18f;
		const FLinearColor Color = GridMapRoomEditor::MarkerColor(Marker.Type);

		FSlateDrawElement::MakeBox(OutDrawElements, ObjectLayer,
			AllottedGeometry.ToPaintGeometry(FVector2f(Cell - Inset * 2.0f, Cell - Inset * 2.0f),
				FSlateLayoutTransform(TopLeft + FVector2f(Inset, Inset))),
			&WhiteBrush, ESlateDrawEffect::None, Color);

		if (Cell >= 14.0f)
		{
			const FString Glyph = GridMapRoomEditor::MarkerGlyph(Marker.Type);
			const FVector2f GlyphSize = FontMeasure->Measure(Glyph, GlyphFont);
			FSlateDrawElement::MakeText(OutDrawElements, TextLayer,
				AllottedGeometry.ToPaintGeometry(GlyphSize,
					FSlateLayoutTransform(TopLeft + FVector2f((Cell - GlyphSize.X) * 0.5f, (Cell - GlyphSize.Y) * 0.5f))),
				Glyph, GlyphFont, ESlateDrawEffect::None, FLinearColor(0.05f, 0.05f, 0.06f));
		}

		if (MarkerIndex == SelectedMarkerIndex)
		{
			BuildCellOutline(LinePoints, TopLeft, Cell);
			FSlateDrawElement::MakeLines(OutDrawElements, CursorLayer, AllottedGeometry.ToPaintGeometry(),
				LinePoints, ESlateDrawEffect::None, FLinearColor::White, true, 2.0f);
		}
	}

	// 问题列表定位高亮
	if (RoomPtr->IsInBounds(HighlightCoord))
	{
		BuildCellOutline(LinePoints, CellToLocal(HighlightCoord), Cell);
		FSlateDrawElement::MakeLines(OutDrawElements, CursorLayer, AllottedGeometry.ToPaintGeometry(),
			LinePoints, ESlateDrawEffect::None, FLinearColor(1.0f, 0.35f, 0.25f), true, 3.0f);
	}

	// 矩形填充预览
	if (DragMode == EDragMode::Rect && IsValidCoord(DragStartCoord) && IsValidCoord(HoverCoord))
	{
		const FIntPoint Min(FMath::Min(DragStartCoord.X, HoverCoord.X), FMath::Min(DragStartCoord.Y, HoverCoord.Y));
		const FIntPoint Max(FMath::Max(DragStartCoord.X, HoverCoord.X), FMath::Max(DragStartCoord.Y, HoverCoord.Y));
		const FVector2f TopLeft = CellToLocal(Min);
		const FVector2f RectSize((Max.X - Min.X + 1) * Cell, (Max.Y - Min.Y + 1) * Cell);

		FSlateDrawElement::MakeBox(OutDrawElements, CursorLayer,
			AllottedGeometry.ToPaintGeometry(RectSize, FSlateLayoutTransform(TopLeft)),
			&WhiteBrush, ESlateDrawEffect::None, FLinearColor(1.0f, 1.0f, 1.0f, 0.18f));

		LinePoints.Reset();
		LinePoints.Add(TopLeft);
		LinePoints.Add(TopLeft + FVector2f(RectSize.X, 0.0f));
		LinePoints.Add(TopLeft + RectSize);
		LinePoints.Add(TopLeft + FVector2f(0.0f, RectSize.Y));
		LinePoints.Add(TopLeft);
		FSlateDrawElement::MakeLines(OutDrawElements, CursorLayer, AllottedGeometry.ToPaintGeometry(),
			LinePoints, ESlateDrawEffect::None, FLinearColor::White, true, 1.5f);
	}
	// 笔刷光标预览
	else if (IsValidCoord(HoverCoord) && DragMode != EDragMode::Pan)
	{
		const EGridMapRoomTool Tool = ActiveTool.Get(EGridMapRoomTool::Brush);
		const int32 Radius = Tool == EGridMapRoomTool::Brush
			? GridMapRoomEditor::BrushRadius(BrushSize.Get(EGridMapRoomBrushSize::One))
			: 0;
		const FVector2f TopLeft = CellToLocal(HoverCoord - FIntPoint(Radius, Radius));
		const float CursorSize = (Radius * 2 + 1) * Cell;

		BuildCellOutline(LinePoints, TopLeft, CursorSize);
		FSlateDrawElement::MakeLines(OutDrawElements, CursorLayer, AllottedGeometry.ToPaintGeometry(),
			LinePoints, ESlateDrawEffect::None, FLinearColor(1.0f, 1.0f, 1.0f, 0.75f), true, 1.5f);
	}

	// 左下角状态：坐标与缩放
	{
		const FString Status = IsValidCoord(HoverCoord)
			? FString::Printf(TEXT("X %d   Y %d       缩放 %d%%"), HoverCoord.X, HoverCoord.Y, FMath::RoundToInt(ZoomLevel * 100.0f))
			: FString::Printf(TEXT("缩放 %d%%"), FMath::RoundToInt(ZoomLevel * 100.0f));
		FSlateDrawElement::MakeText(OutDrawElements, TextLayer,
			AllottedGeometry.ToPaintGeometry(FVector2f(280.0f, 16.0f), FSlateLayoutTransform(FVector2f(8.0f, ViewSize.Y - 20.0f))),
			Status, HintFont, ESlateDrawEffect::None, FLinearColor(0.75f, 0.78f, 0.82f));
	}

	return TextLayer + 1;
}

// ---------------------------------------------------------------------------
// 事务
// ---------------------------------------------------------------------------

void SGridMapRoomCanvas::BeginStroke(const FText& Description)
{
	UGridMapRoomAsset* RoomPtr = Room.Get();
	if (!RoomPtr || ActiveTransaction.IsValid())
	{
		return;
	}
	// 一次连续笔划只开一个事务，撤销时整笔回滚
	ActiveTransaction = MakeUnique<FScopedTransaction>(Description);
	bStrokeModified = false;
	RoomPtr->Modify();
}

void SGridMapRoomCanvas::EndStroke()
{
	if (!ActiveTransaction.IsValid())
	{
		return;
	}
	if (!bStrokeModified)
	{
		ActiveTransaction->Cancel();
	}
	ActiveTransaction.Reset();

	if (bStrokeModified)
	{
		if (UGridMapRoomAsset* RoomPtr = Room.Get())
		{
			RoomPtr->MarkPackageDirty();
		}
		NotifyModified();
	}
	bStrokeModified = false;
}

void SGridMapRoomCanvas::NotifyModified()
{
	RefreshReachability();
	OnRoomModified.ExecuteIfBound();
}

// ---------------------------------------------------------------------------
// 工具行为
// ---------------------------------------------------------------------------

void SGridMapRoomCanvas::ApplyBrush(const FIntPoint Coord, const EGridTileType Tile)
{
	UGridMapRoomAsset* RoomPtr = Room.Get();
	if (!RoomPtr)
	{
		return;
	}
	const int32 Radius = GridMapRoomEditor::BrushRadius(BrushSize.Get(EGridMapRoomBrushSize::One));
	for (int32 OffsetY = -Radius; OffsetY <= Radius; ++OffsetY)
	{
		for (int32 OffsetX = -Radius; OffsetX <= Radius; ++OffsetX)
		{
			// SetTile 自带边界检查，越界直接返回 false
			const FIntPoint Target = Coord + FIntPoint(OffsetX, OffsetY);
			if (RoomPtr->IsInBounds(Target) && RoomPtr->GetTile(Target) != Tile)
			{
				RoomPtr->SetTile(Target, Tile);
				bStrokeModified = true;
			}
		}
	}
}

void SGridMapRoomCanvas::ApplyBrushLine(const FIntPoint From, const FIntPoint To, const EGridTileType Tile)
{
	// 鼠标快速移动时补齐中间格，避免笔划出现断点
	const int32 Steps = FMath::Max(FMath::Abs(To.X - From.X), FMath::Abs(To.Y - From.Y));
	if (Steps <= 0)
	{
		ApplyBrush(To, Tile);
		return;
	}
	for (int32 Step = 0; Step <= Steps; ++Step)
	{
		const float Alpha = static_cast<float>(Step) / static_cast<float>(Steps);
		const FIntPoint Coord(
			FMath::RoundToInt32(FMath::Lerp(static_cast<float>(From.X), static_cast<float>(To.X), Alpha)),
			FMath::RoundToInt32(FMath::Lerp(static_cast<float>(From.Y), static_cast<float>(To.Y), Alpha)));
		ApplyBrush(Coord, Tile);
	}
}

void SGridMapRoomCanvas::ApplyRect(const FIntPoint A, const FIntPoint B, const EGridTileType Tile)
{
	UGridMapRoomAsset* RoomPtr = Room.Get();
	if (!RoomPtr)
	{
		return;
	}
	const int32 MinX = FMath::Min(A.X, B.X);
	const int32 MaxX = FMath::Max(A.X, B.X);
	const int32 MinY = FMath::Min(A.Y, B.Y);
	const int32 MaxY = FMath::Max(A.Y, B.Y);
	for (int32 Y = MinY; Y <= MaxY; ++Y)
	{
		for (int32 X = MinX; X <= MaxX; ++X)
		{
			const FIntPoint Target(X, Y);
			if (RoomPtr->IsInBounds(Target) && RoomPtr->GetTile(Target) != Tile)
			{
				RoomPtr->SetTile(Target, Tile);
				bStrokeModified = true;
			}
		}
	}
}

void SGridMapRoomCanvas::ApplyFloodFill(const FIntPoint Coord, const EGridTileType Tile)
{
	using namespace GridMapRoomCanvasLocal;

	UGridMapRoomAsset* RoomPtr = Room.Get();
	if (!RoomPtr || !RoomPtr->IsInBounds(Coord))
	{
		return;
	}
	const EGridTileType SourceTile = RoomPtr->GetTile(Coord);
	if (SourceTile == Tile)
	{
		return;
	}

	TArray<FIntPoint> Queue;
	TSet<FIntPoint> Visited;
	Queue.Add(Coord);
	Visited.Add(Coord);

	for (int32 Head = 0; Head < Queue.Num(); ++Head)
	{
		const FIntPoint Current = Queue[Head];
		RoomPtr->SetTile(Current, Tile);
		bStrokeModified = true;
		for (const FIntPoint Step : NeighborSteps)
		{
			const FIntPoint Next = Current + Step;
			if (RoomPtr->IsInBounds(Next) && !Visited.Contains(Next) && RoomPtr->GetTile(Next) == SourceTile)
			{
				Visited.Add(Next);
				Queue.Add(Next);
			}
		}
	}
}

void SGridMapRoomCanvas::ApplyMarker(const FIntPoint Coord, const bool bErase)
{
	UGridMapRoomAsset* RoomPtr = Room.Get();
	if (!RoomPtr || !RoomPtr->IsInBounds(Coord))
	{
		return;
	}

	const int32 Existing = FindMarkerAt(Coord);
	if (bErase)
	{
		if (Existing != INDEX_NONE)
		{
			RoomPtr->Markers.RemoveAt(Existing);
			bStrokeModified = true;
			ClearSelection();
		}
		return;
	}

	const EGridMapMarkerType Type = MarkerType.Get(EGridMapMarkerType::Decoration);
	if (Existing != INDEX_NONE)
	{
		// 同格已有标记：改类型而不是叠加
		if (RoomPtr->Markers[Existing].Type != Type)
		{
			RoomPtr->Markers[Existing].Type = Type;
			bStrokeModified = true;
		}
		SelectedMarkerIndex = Existing;
	}
	else
	{
		FGridMapRoomMarker& NewMarker = RoomPtr->Markers.AddDefaulted_GetRef();
		NewMarker.Type = Type;
		NewMarker.GridCoord = Coord;
		bStrokeModified = true;
		SelectedMarkerIndex = RoomPtr->Markers.Num() - 1;
	}
	SelectedExitIndex = INDEX_NONE;
	OnCanvasSelectionChanged.ExecuteIfBound();
}

bool SGridMapRoomCanvas::IsCoordOnEdge(const FIntPoint Coord, EGridMapExitDirection& OutDirection) const
{
	const UGridMapRoomAsset* RoomPtr = Room.Get();
	if (!RoomPtr || !RoomPtr->IsInBounds(Coord))
	{
		return false;
	}
	// 角落格同时属于两条边，优先取水平边
	if (Coord.Y == RoomPtr->Size.Y - 1)
	{
		OutDirection = EGridMapExitDirection::North;
		return true;
	}
	if (Coord.Y == 0)
	{
		OutDirection = EGridMapExitDirection::South;
		return true;
	}
	if (Coord.X == RoomPtr->Size.X - 1)
	{
		OutDirection = EGridMapExitDirection::East;
		return true;
	}
	if (Coord.X == 0)
	{
		OutDirection = EGridMapExitDirection::West;
		return true;
	}
	return false;
}

bool SGridMapRoomCanvas::ExitCoversCoord(const FGridMapRoomExit& Exit, const FIntPoint Coord)
{
	const FIntPoint Step = GridMapRoomEditor::ExitSpanStep(Exit.Direction);
	for (int32 Offset = 0; Offset < FMath::Max(Exit.Width, 1); ++Offset)
	{
		if (Exit.Start + Step * Offset == Coord)
		{
			return true;
		}
	}
	return false;
}

int32 SGridMapRoomCanvas::FindExitAt(const FIntPoint Coord) const
{
	const UGridMapRoomAsset* RoomPtr = Room.Get();
	if (!RoomPtr)
	{
		return INDEX_NONE;
	}
	for (int32 Index = 0; Index < RoomPtr->Exits.Num(); ++Index)
	{
		if (ExitCoversCoord(RoomPtr->Exits[Index], Coord))
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

int32 SGridMapRoomCanvas::FindMarkerAt(const FIntPoint Coord) const
{
	const UGridMapRoomAsset* RoomPtr = Room.Get();
	if (!RoomPtr)
	{
		return INDEX_NONE;
	}
	for (int32 Index = 0; Index < RoomPtr->Markers.Num(); ++Index)
	{
		if (RoomPtr->Markers[Index].GridCoord == Coord)
		{
			return Index;
		}
	}
	return INDEX_NONE;
}

void SGridMapRoomCanvas::HandleExitPress(const FIntPoint Coord, const bool bErase, const bool bResizeModifier)
{
	UGridMapRoomAsset* RoomPtr = Room.Get();
	if (!RoomPtr)
	{
		return;
	}

	const int32 ExistingIndex = FindExitAt(Coord);

	if (bErase)
	{
		if (ExistingIndex != INDEX_NONE)
		{
			BeginStroke(LOCTEXT("DeleteExit", "删除房间出口"));
			RoomPtr->Exits.RemoveAt(ExistingIndex);
			bStrokeModified = true;
			ClearSelection();
			EndStroke();
		}
		return;
	}

	if (ExistingIndex != INDEX_NONE)
	{
		// 命中已有出口：默认拖动位置，按住 Shift 改宽度
		SelectedExitIndex = ExistingIndex;
		SelectedMarkerIndex = INDEX_NONE;
		OnCanvasSelectionChanged.ExecuteIfBound();
		BeginStroke(bResizeModifier ? LOCTEXT("ResizeExit", "调整出口宽度") : LOCTEXT("MoveExit", "移动房间出口"));
		DragMode = bResizeModifier ? EDragMode::ExitResize : EDragMode::ExitMove;
		return;
	}

	EGridMapExitDirection Direction = EGridMapExitDirection::North;
	if (!IsCoordOnEdge(Coord, Direction))
	{
		return;
	}

	// 边缘空位：新建一个 1 格宽出口并立即进入拖拽
	BeginStroke(LOCTEXT("CreateExit", "创建房间出口"));
	FGridMapRoomExit& NewExit = RoomPtr->Exits.AddDefaulted_GetRef();
	NewExit.Id = GridMapRoomCanvasLocal::MakeUniqueExitId(*RoomPtr);
	NewExit.Direction = Direction;
	NewExit.Start = Coord;
	NewExit.Width = 1;
	bStrokeModified = true;

	SelectedExitIndex = RoomPtr->Exits.Num() - 1;
	SelectedMarkerIndex = INDEX_NONE;
	OnCanvasSelectionChanged.ExecuteIfBound();
	DragMode = EDragMode::ExitMove;
}

void SGridMapRoomCanvas::DragExitTo(const FIntPoint Coord)
{
	UGridMapRoomAsset* RoomPtr = Room.Get();
	if (!RoomPtr || !RoomPtr->Exits.IsValidIndex(SelectedExitIndex) || !RoomPtr->IsInBounds(Coord))
	{
		return;
	}

	FGridMapRoomExit& Exit = RoomPtr->Exits[SelectedExitIndex];
	const bool bHorizontalEdge = Exit.Direction == EGridMapExitDirection::North || Exit.Direction == EGridMapExitDirection::South;
	// 出口始终锁在自己那条边上，只允许沿边缘滑动
	const int32 AxisMax = bHorizontalEdge ? RoomPtr->Size.X : RoomPtr->Size.Y;
	const int32 AxisValue = bHorizontalEdge ? Coord.X : Coord.Y;

	if (DragMode == EDragMode::ExitResize)
	{
		const int32 AnchorValue = bHorizontalEdge ? Exit.Start.X : Exit.Start.Y;
		const int32 NewWidth = FMath::Clamp(AxisValue - AnchorValue + 1, 1, FMath::Max(AxisMax - AnchorValue, 1));
		if (Exit.Width != NewWidth)
		{
			Exit.Width = NewWidth;
			bStrokeModified = true;
		}
	}
	else
	{
		const int32 NewStart = FMath::Clamp(AxisValue, 0, FMath::Max(AxisMax - Exit.Width, 0));
		if (bHorizontalEdge)
		{
			if (Exit.Start.X != NewStart)
			{
				Exit.Start.X = NewStart;
				bStrokeModified = true;
			}
		}
		else
		{
			if (Exit.Start.Y != NewStart)
			{
				Exit.Start.Y = NewStart;
				bStrokeModified = true;
			}
		}
	}
}

void SGridMapRoomCanvas::ClearSelection()
{
	SelectedMarkerIndex = INDEX_NONE;
	SelectedExitIndex = INDEX_NONE;
	OnCanvasSelectionChanged.ExecuteIfBound();
}

// ---------------------------------------------------------------------------
// 输入
// ---------------------------------------------------------------------------

FReply SGridMapRoomCanvas::OnMouseButtonDown(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	UGridMapRoomAsset* RoomPtr = Room.Get();
	if (!RoomPtr)
	{
		return FReply::Unhandled();
	}

	const FIntPoint Coord = LocalToCell(MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition()));
	HoverCoord = Coord;
	HighlightCoord = FIntPoint(INDEX_NONE, INDEX_NONE);

	const bool bLeft = MouseEvent.GetEffectingButton() == EKeys::LeftMouseButton;
	const bool bRight = MouseEvent.GetEffectingButton() == EKeys::RightMouseButton;
	const bool bMiddle = MouseEvent.GetEffectingButton() == EKeys::MiddleMouseButton;

	// 中键，或 Space + 左键 → 平移
	if (bMiddle || (bLeft && bSpaceDown))
	{
		DragMode = EDragMode::Pan;
		return FReply::Handled().CaptureMouse(SharedThis(this)).SetUserFocus(SharedThis(this), EFocusCause::Mouse);
	}

	if (!bLeft && !bRight)
	{
		return FReply::Unhandled();
	}
	if (!RoomPtr->IsInBounds(Coord))
	{
		return FReply::Handled().SetUserFocus(SharedThis(this), EFocusCause::Mouse);
	}

	const EGridMapRoomTool Tool = ActiveTool.Get(EGridMapRoomTool::Brush);
	// 右键统一表示"擦除"：地形恢复空地，标记 / 出口删除
	bEraseDrag = bRight;
	DragStartCoord = Coord;
	LastPaintedCoord = Coord;

	switch (Tool)
	{
	case EGridMapRoomTool::Eyedropper:
		if (!bRight)
		{
			OnTilePicked.ExecuteIfBound(RoomPtr->GetTile(Coord));
		}
		break;

	case EGridMapRoomTool::Exit:
		HandleExitPress(Coord, bRight, MouseEvent.IsShiftDown());
		break;

	case EGridMapRoomTool::Marker:
	{
		const int32 ExistingMarker = FindMarkerAt(Coord);
		if (!bRight && ExistingMarker != INDEX_NONE)
		{
			// 点到已有标记：进入拖动模式
			SelectedMarkerIndex = ExistingMarker;
			SelectedExitIndex = INDEX_NONE;
			OnCanvasSelectionChanged.ExecuteIfBound();
			BeginStroke(LOCTEXT("MoveMarker", "移动语义标记"));
			DragMode = EDragMode::MarkerMove;
		}
		else
		{
			BeginStroke(bRight ? LOCTEXT("EraseMarker", "删除语义标记") : LOCTEXT("PaintMarker", "放置语义标记"));
			ApplyMarker(Coord, bRight);
			DragMode = EDragMode::Paint;
		}
		break;
	}

	case EGridMapRoomTool::RectFill:
		DragMode = EDragMode::Rect;
		break;

	case EGridMapRoomTool::FloodFill:
		BeginStroke(LOCTEXT("FloodFillTiles", "区域填充地形"));
		ApplyFloodFill(Coord, bRight ? EGridTileType::Empty : PaintTile.Get(EGridTileType::Empty));
		EndStroke();
		break;

	case EGridMapRoomTool::Brush:
	default:
		BeginStroke(bRight ? LOCTEXT("EraseTiles", "擦除地形") : LOCTEXT("PaintTiles", "绘制地形"));
		ApplyBrush(Coord, bRight ? EGridTileType::Empty : PaintTile.Get(EGridTileType::Empty));
		DragMode = EDragMode::Paint;
		break;
	}

	FReply Reply = FReply::Handled().SetUserFocus(SharedThis(this), EFocusCause::Mouse);
	return DragMode != EDragMode::None ? Reply.CaptureMouse(SharedThis(this)) : Reply;
}

FReply SGridMapRoomCanvas::OnMouseMove(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	UGridMapRoomAsset* RoomPtr = Room.Get();
	if (!RoomPtr)
	{
		return FReply::Unhandled();
	}

	const FIntPoint Coord = LocalToCell(MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition()));
	const bool bCoordChanged = Coord != HoverCoord;
	HoverCoord = Coord;

	switch (DragMode)
	{
	case EDragMode::Pan:
		PanOffset += MouseEvent.GetCursorDelta();
		return FReply::Handled();

	case EDragMode::Paint:
		if (bCoordChanged && RoomPtr->IsInBounds(Coord))
		{
			if (ActiveTool.Get(EGridMapRoomTool::Brush) == EGridMapRoomTool::Marker)
			{
				ApplyMarker(Coord, bEraseDrag);
			}
			else
			{
				ApplyBrushLine(LastPaintedCoord, Coord, bEraseDrag ? EGridTileType::Empty : PaintTile.Get(EGridTileType::Empty));
			}
			LastPaintedCoord = Coord;
		}
		return FReply::Handled();

	case EDragMode::MarkerMove:
		if (bCoordChanged && RoomPtr->IsInBounds(Coord) && RoomPtr->Markers.IsValidIndex(SelectedMarkerIndex))
		{
			if (RoomPtr->Markers[SelectedMarkerIndex].GridCoord != Coord)
			{
				RoomPtr->Markers[SelectedMarkerIndex].GridCoord = Coord;
				bStrokeModified = true;
			}
		}
		return FReply::Handled();

	case EDragMode::ExitMove:
	case EDragMode::ExitResize:
		if (bCoordChanged)
		{
			DragExitTo(Coord);
		}
		return FReply::Handled();

	default:
		break;
	}

	return FReply::Handled();
}

FReply SGridMapRoomCanvas::OnMouseButtonUp(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	if (DragMode == EDragMode::None)
	{
		return HasMouseCapture() ? FReply::Handled().ReleaseMouseCapture() : FReply::Unhandled();
	}

	if (DragMode == EDragMode::Rect)
	{
		UGridMapRoomAsset* RoomPtr = Room.Get();
		if (RoomPtr && RoomPtr->IsInBounds(DragStartCoord) && RoomPtr->IsInBounds(HoverCoord))
		{
			BeginStroke(bEraseDrag ? LOCTEXT("RectErase", "矩形擦除地形") : LOCTEXT("RectFill", "矩形填充地形"));
			ApplyRect(DragStartCoord, HoverCoord, bEraseDrag ? EGridTileType::Empty : PaintTile.Get(EGridTileType::Empty));
			EndStroke();
		}
	}
	else if (DragMode != EDragMode::Pan)
	{
		EndStroke();
	}

	DragMode = EDragMode::None;
	bEraseDrag = false;
	FReply Reply = FReply::Handled();
	return HasMouseCapture() ? Reply.ReleaseMouseCapture() : Reply;
}

FReply SGridMapRoomCanvas::OnMouseWheel(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	const float Delta = MouseEvent.GetWheelDelta();
	if (FMath::IsNearlyZero(Delta))
	{
		return FReply::Unhandled();
	}

	const FVector2f LocalPos = MyGeometry.AbsoluteToLocal(MouseEvent.GetScreenSpacePosition());
	const float OldZoom = ZoomLevel;
	ZoomLevel = FMath::Clamp(ZoomLevel * (Delta > 0.0f ? 1.15f : 1.0f / 1.15f), 0.15f, 6.0f);

	// 以光标为锚点缩放，保证指针下的格子不漂移
	if (!FMath::IsNearlyEqual(OldZoom, ZoomLevel))
	{
		const FVector2f GridPos = (LocalPos - PanOffset) / (BaseCellSize * OldZoom);
		PanOffset = LocalPos - GridPos * (BaseCellSize * ZoomLevel);
	}
	return FReply::Handled();
}

FReply SGridMapRoomCanvas::OnKeyDown(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::SpaceBar)
	{
		bSpaceDown = true;
		return FReply::Handled();
	}
	if (InKeyEvent.GetKey() == EKeys::F)
	{
		ResetView();
		return FReply::Handled();
	}
	// Ctrl+Z / Ctrl+Y 交给 Toolkit 的命令列表处理
	return FReply::Unhandled();
}

FReply SGridMapRoomCanvas::OnKeyUp(const FGeometry& MyGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::SpaceBar)
	{
		bSpaceDown = false;
		return FReply::Handled();
	}
	return FReply::Unhandled();
}

void SGridMapRoomCanvas::OnMouseEnter(const FGeometry& MyGeometry, const FPointerEvent& MouseEvent)
{
	SLeafWidget::OnMouseEnter(MyGeometry, MouseEvent);
	// 进入画布即取得键盘焦点，确保从其他面板移入后 Space + 左键仍可直接平移。
	FSlateApplication::Get().SetKeyboardFocus(SharedThis(this), EFocusCause::Mouse);
}

void SGridMapRoomCanvas::OnMouseLeave(const FPointerEvent& MouseEvent)
{
	SLeafWidget::OnMouseLeave(MouseEvent);
	HoverCoord = FIntPoint(INDEX_NONE, INDEX_NONE);
	bSpaceDown = false;
}

// ---------------------------------------------------------------------------
// 外部接口
// ---------------------------------------------------------------------------

void SGridMapRoomCanvas::RefreshFromAsset()
{
	// 撤销 / 重做或尺寸变化后，之前记录的索引可能已经越界
	UGridMapRoomAsset* RoomPtr = Room.Get();
	if (!RoomPtr || !RoomPtr->Markers.IsValidIndex(SelectedMarkerIndex))
	{
		SelectedMarkerIndex = INDEX_NONE;
	}
	if (!RoomPtr || !RoomPtr->Exits.IsValidIndex(SelectedExitIndex))
	{
		SelectedExitIndex = INDEX_NONE;
	}
	if (!RoomPtr || !RoomPtr->IsInBounds(HighlightCoord))
	{
		HighlightCoord = FIntPoint(INDEX_NONE, INDEX_NONE);
	}

	DragMode = EDragMode::None;
	ActiveTransaction.Reset();
	bStrokeModified = false;
	RefreshReachability();
	OnCanvasSelectionChanged.ExecuteIfBound();
}

void SGridMapRoomCanvas::RefreshReachability()
{
	using namespace GridMapRoomCanvasLocal;

	ReachableCells.Reset();

	const UGridMapRoomAsset* RoomPtr = Room.Get();
	if (!RoomPtr || RoomPtr->Size.X <= 0 || RoomPtr->Size.Y <= 0)
	{
		return;
	}
	if (static_cast<int64>(RoomPtr->Tiles.Num()) != static_cast<int64>(RoomPtr->Size.X) * RoomPtr->Size.Y)
	{
		return;
	}

	auto IsPassable = [RoomPtr](const FIntPoint Coord)
	{
		return RoomPtr->IsInBounds(Coord) && RoomPtr->GetTile(Coord) == EGridTileType::Empty;
	};

	// 起点优先取第一个有效出口 Start 向房间内部一格，与 Validator 的判定口径一致；
	// 没有可用出口时退化为第一块空地，便于草稿阶段也能看到连通情况。
	FIntPoint Start(INDEX_NONE, INDEX_NONE);
	for (const FGridMapRoomExit& Exit : RoomPtr->Exits)
	{
		const bool bHorizontal = Exit.Direction == EGridMapExitDirection::North || Exit.Direction == EGridMapExitDirection::South;
		const bool bOnExpectedEdge = bHorizontal
			? Exit.Start.Y == (Exit.Direction == EGridMapExitDirection::North ? RoomPtr->Size.Y - 1 : 0)
			: Exit.Start.X == (Exit.Direction == EGridMapExitDirection::East ? RoomPtr->Size.X - 1 : 0);
		const int32 AxisStart = bHorizontal ? Exit.Start.X : Exit.Start.Y;
		const int32 AxisMax = bHorizontal ? RoomPtr->Size.X : RoomPtr->Size.Y;
		if (Exit.Width <= 0 || !bOnExpectedEdge || AxisStart < 0 || AxisStart + Exit.Width > AxisMax)
		{
			continue;
		}
		FIntPoint InwardStep = FIntPoint::ZeroValue;
		switch (Exit.Direction)
		{
		case EGridMapExitDirection::North: InwardStep = FIntPoint(0, -1); break;
		case EGridMapExitDirection::East: InwardStep = FIntPoint(-1, 0); break;
		case EGridMapExitDirection::South: InwardStep = FIntPoint(0, 1); break;
		case EGridMapExitDirection::West: InwardStep = FIntPoint(1, 0); break;
		default: continue;
		}
		const FIntPoint Candidate = Exit.Start + InwardStep;
		if (Exit.Width > 0 && IsPassable(Candidate))
		{
			Start = Candidate;
			break;
		}
	}
	if (Start.X == INDEX_NONE)
	{
		for (int32 Y = 0; Y < RoomPtr->Size.Y && Start.X == INDEX_NONE; ++Y)
		{
			for (int32 X = 0; X < RoomPtr->Size.X; ++X)
			{
				if (IsPassable(FIntPoint(X, Y)))
				{
					Start = FIntPoint(X, Y);
					break;
				}
			}
		}
	}
	if (Start.X == INDEX_NONE)
	{
		return;
	}

	TArray<FIntPoint> Queue;
	Queue.Reserve(RoomPtr->Tiles.Num());
	Queue.Add(Start);
	ReachableCells.Add(Start);

	for (int32 Head = 0; Head < Queue.Num(); ++Head)
	{
		for (const FIntPoint Step : NeighborSteps)
		{
			const FIntPoint Next = Queue[Head] + Step;
			if (IsPassable(Next) && !ReachableCells.Contains(Next))
			{
				ReachableCells.Add(Next);
				Queue.Add(Next);
			}
		}
	}
}

void SGridMapRoomCanvas::FocusOnCoord(const FIntPoint Coord)
{
	if (!IsValidCoord(Coord))
	{
		return;
	}
	HighlightCoord = Coord;

	const FVector2f ViewSize = GetTickSpaceGeometry().GetLocalSize();
	if (ViewSize.X > 1.0f && ViewSize.Y > 1.0f)
	{
		const float Cell = GetCellSize();
		PanOffset = ViewSize * 0.5f - FVector2f((Coord.X + 0.5f) * Cell, (Coord.Y + 0.5f) * Cell);
	}
}

void SGridMapRoomCanvas::ResetView()
{
	const UGridMapRoomAsset* RoomPtr = Room.Get();
	const FVector2f ViewSize = GetTickSpaceGeometry().GetLocalSize();
	if (!RoomPtr || RoomPtr->Size.X <= 0 || RoomPtr->Size.Y <= 0 || ViewSize.X <= 1.0f || ViewSize.Y <= 1.0f)
	{
		ZoomLevel = 1.0f;
		PanOffset = FVector2f(24.0f, 24.0f);
		return;
	}

	// 留出边距后按短边适配整张房间
	const float FitX = (ViewSize.X - 48.0f) / (RoomPtr->Size.X * BaseCellSize);
	const float FitY = (ViewSize.Y - 48.0f) / (RoomPtr->Size.Y * BaseCellSize);
	ZoomLevel = FMath::Clamp(FMath::Min(FitX, FitY), 0.15f, 6.0f);

	const float Cell = GetCellSize();
	PanOffset = (ViewSize - FVector2f(RoomPtr->Size.X * Cell, RoomPtr->Size.Y * Cell)) * 0.5f;
}

FText SGridMapRoomCanvas::GetSelectionInfoText() const
{
	const UGridMapRoomAsset* RoomPtr = Room.Get();
	if (!RoomPtr)
	{
		return LOCTEXT("NoAsset", "未加载房间资产。");
	}

	if (RoomPtr->Exits.IsValidIndex(SelectedExitIndex))
	{
		const FGridMapRoomExit& Exit = RoomPtr->Exits[SelectedExitIndex];
		return FText::FromString(FString::Printf(TEXT("出口  %s\n方向：%s\n起点：(%d, %d)\n宽度：%d 格"),
			*Exit.Id.ToString(),
			*GridMapRoomEditor::ExitDirectionLabel(Exit.Direction).ToString(),
			Exit.Start.X, Exit.Start.Y, Exit.Width));
	}

	if (RoomPtr->Markers.IsValidIndex(SelectedMarkerIndex))
	{
		const FGridMapRoomMarker& Marker = RoomPtr->Markers[SelectedMarkerIndex];
		return FText::FromString(FString::Printf(TEXT("标记  %s\n坐标：(%d, %d)\n朝向 Yaw：%.1f\n分组：%s\n权重：%.2f"),
			*GridMapRoomEditor::MarkerLabel(Marker.Type).ToString(),
			Marker.GridCoord.X, Marker.GridCoord.Y,
			Marker.Yaw,
			Marker.Group.IsNone() ? TEXT("-") : *Marker.Group.ToString(),
			Marker.Weight));
	}

	return LOCTEXT("NoSelection", "当前没有选中的出口或标记。\n用「出口」或「标记」工具在画布上点击即可选中。");
}

#undef LOCTEXT_NAMESPACE
