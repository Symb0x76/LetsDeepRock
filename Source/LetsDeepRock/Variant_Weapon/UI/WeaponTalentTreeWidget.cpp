// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponTalentTreeWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/Image.h"
#include "Components/ScaleBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "GameFramework/PlayerController.h"
#include "InputCoreTypes.h"
#include "Styling/CoreStyle.h"
#include "Styling/SlateBrush.h"
#include "Styling/SlateTypes.h"

namespace WeaponTalentStyle
{
	static const FLinearColor Background(0.012f, 0.016f, 0.022f, 0.985f);
	static const FLinearColor Locked(0.065f, 0.073f, 0.082f, 1.0f);
	static const FLinearColor Available(0.19f, 0.21f, 0.23f, 1.0f);
	static const FLinearColor Amber(1.0f, 0.48f, 0.055f, 1.0f);
	static const FLinearColor AmberHover(1.0f, 0.64f, 0.16f, 1.0f);
	static const FLinearColor LineDim(0.17f, 0.18f, 0.19f, 0.72f);
	static const FLinearColor MutexHint(0.62f, 0.18f, 0.16f, 0.45f);

	FSlateBrush MakeBrush(const FLinearColor& Color, float Radius = 0.0f)
	{
		FSlateBrush Brush;
		Brush.DrawAs = ESlateBrushDrawType::RoundedBox;
		Brush.TintColor = FSlateColor(Color);
		Brush.OutlineSettings.RoundingType = ESlateBrushRoundingType::FixedRadius;
		Brush.OutlineSettings.CornerRadii = FVector4(Radius, Radius, Radius, Radius);
		return Brush;
	}

	/** bInteractive 为 false 时（锁定 / 互斥排除 / 已激活）悬浮只做极弱提亮，不给可点击暗示。 */
	FButtonStyle MakeButtonStyle(const FLinearColor& Normal, float Radius, bool bInteractive)
	{
		FButtonStyle Style;
		Style.SetNormal(MakeBrush(Normal, Radius));
		Style.SetHovered(MakeBrush(bInteractive ? AmberHover : Normal * 1.18f, Radius));
		Style.SetPressed(MakeBrush(bInteractive ? Amber : Normal, Radius));
		Style.SetDisabled(MakeBrush(Normal, Radius));
		Style.SetNormalPadding(FMargin(0.0f));
		Style.SetPressedPadding(FMargin(0.0f));
		return Style;
	}
}

void UWeaponTalentNodeButton::InitializeNode(int32 InNodeId, UWeaponTalentTreeWidget* InOwner)
{
	NodeId = InNodeId;
	OwnerWidget = InOwner;
	OnClicked.AddUniqueDynamic(this, &UWeaponTalentNodeButton::HandleClicked);
}

void UWeaponTalentNodeButton::HandleClicked()
{
	if (OwnerWidget)
	{
		OwnerWidget->HandleNodeClicked(NodeId);
	}
}

void UWeaponTalentTreeWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	SetIsFocusable(true);
	BuildLayout();
}

void UWeaponTalentTreeWidget::ShowTalentTree()
{
	if (!IsInViewport())
	{
		AddToViewport(100);
	}
	SetVisibility(ESlateVisibility::Visible);
	SetKeyboardFocus();

	if (APlayerController* PC = GetOwningPlayer())
	{
		FInputModeGameAndUI InputMode;
		InputMode.SetWidgetToFocus(TakeWidget());
		InputMode.SetHideCursorDuringCapture(false);
		PC->SetInputMode(InputMode);
		PC->SetShowMouseCursor(true);
	}
}

void UWeaponTalentTreeWidget::CloseTalentTree()
{
	RemoveFromParent();
	if (APlayerController* PC = GetOwningPlayer())
	{
		FInputModeGameAndUI InputMode;
		PC->SetInputMode(InputMode);
		PC->SetShowMouseCursor(true);
	}
}

FReply UWeaponTalentTreeWidget::NativeOnKeyDown(const FGeometry& InGeometry, const FKeyEvent& InKeyEvent)
{
	if (InKeyEvent.GetKey() == EKeys::Escape)
	{
		CloseTalentTree();
		return FReply::Handled();
	}
	return Super::NativeOnKeyDown(InGeometry, InKeyEvent);
}

void UWeaponTalentTreeWidget::BuildLayout()
{
	using namespace WeaponTalentStyle;
	if (!WidgetTree)
	{
		return;
	}

	UCanvasPanel* Root = WidgetTree->ConstructWidget<UCanvasPanel>();
	WidgetTree->RootWidget = Root;

	UBorder* Backdrop = WidgetTree->ConstructWidget<UBorder>();
	Backdrop->SetBrushColor(Background);
	UCanvasPanelSlot* BackdropSlot = Root->AddChildToCanvas(Backdrop);
	BackdropSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
	BackdropSlot->SetOffsets(FMargin(0.0f));

	UScaleBox* Scale = WidgetTree->ConstructWidget<UScaleBox>();
	Scale->SetStretch(EStretch::ScaleToFit);
	UCanvasPanelSlot* ScaleSlot = Root->AddChildToCanvas(Scale);
	ScaleSlot->SetAnchors(FAnchors(0.0f, 0.0f, 1.0f, 1.0f));
	ScaleSlot->SetOffsets(FMargin(0.0f));

	USizeBox* DesignSize = WidgetTree->ConstructWidget<USizeBox>();
	DesignSize->SetWidthOverride(1760.0f);
	DesignSize->SetHeightOverride(940.0f);
	Scale->AddChild(DesignSize);

	TreeCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();
	DesignSize->AddChild(TreeCanvas);

	TalentPointsText = WidgetTree->ConstructWidget<UTextBlock>();
	TalentPointsText->SetText(FText::FromString(TEXT("天赋点 999")));
	TalentPointsText->SetColorAndOpacity(FSlateColor(FLinearColor(0.93f, 0.94f, 0.95f, 1.0f)));
	TalentPointsText->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 26));
	UCanvasPanelSlot* PointsSlot = TreeCanvas->AddChildToCanvas(TalentPointsText);
	PointsSlot->SetPosition(FVector2D(54.0f, 34.0f));
	PointsSlot->SetAutoSize(true);
	PointsSlot->SetZOrder(5);

	UButton* CloseButton = WidgetTree->ConstructWidget<UButton>();
	CloseButton->SetStyle(MakeButtonStyle(FLinearColor(0.12f, 0.13f, 0.15f, 1.0f), 3.0f, true));
	CloseButton->OnClicked.AddDynamic(this, &UWeaponTalentTreeWidget::HandleCloseClicked);
	UTextBlock* CloseText = WidgetTree->ConstructWidget<UTextBlock>();
	CloseText->SetText(FText::FromString(TEXT("返回  ESC")));
	CloseText->SetColorAndOpacity(FSlateColor(FLinearColor::White));
	CloseText->SetFont(FCoreStyle::GetDefaultFontStyle(TEXT("Bold"), 16));
	CloseButton->AddChild(CloseText);
	UCanvasPanelSlot* CloseSlot = TreeCanvas->AddChildToCanvas(CloseButton);
	CloseSlot->SetPosition(FVector2D(1560.0f, 30.0f));
	CloseSlot->SetSize(FVector2D(150.0f, 48.0f));
	CloseSlot->SetZOrder(5);

	InitializeTopology();
	AddMutexHintLines();
	for (FWeaponTalentConnection& Connection : Connections)
	{
		CreateConnectionWidget(Connection);
	}
	for (TPair<int32, FWeaponTalentNode>& Pair : Nodes)
	{
		CreateNodeWidget(Pair.Value);
	}
	RefreshVisuals();
}

int32 UWeaponTalentTreeWidget::AddNode(EWeaponTalentNodeShape Shape, const FVector2D& Position, const TArray<int32>& Parents, int32 MutexGroup)
{
	FWeaponTalentNode Node;
	Node.Id = NextNodeId++;
	Node.Shape = Shape;
	Node.Position = Position;
	Node.Parents = Parents;
	Node.MutexGroup = MutexGroup;
	Nodes.Add(Node.Id, Node);
	for (int32 Parent : Parents)
	{
		AddConnection(Parent, Node.Id);
	}
	return Node.Id;
}

void UWeaponTalentTreeWidget::AddConnection(int32 FromId, int32 ToId, bool bMutexHint)
{
	FWeaponTalentConnection& Connection = Connections.AddDefaulted_GetRef();
	Connection.FromId = FromId;
	Connection.ToId = ToId;
	Connection.bMutexHint = bMutexHint;
}

void UWeaponTalentTreeWidget::InitializeTopology()
{
	Nodes.Reset();
	Connections.Reset();
	NextNodeId = 1;
	const float Center = 880.0f;

	const int32 Root = AddNode(EWeaponTalentNodeShape::Circle, FVector2D(Center, 94.0f));
	TArray<int32> FourA;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		FourA.Add(AddNode(EWeaponTalentNodeShape::Circle, FVector2D(Center + (Index - 1.5f) * 150.0f, 190.0f), {Root}));
	}
	const int32 HubA = AddNode(EWeaponTalentNodeShape::Hub, FVector2D(Center, 286.0f), FourA);

	TArray<int32> Squares;
	for (int32 Index = 0; Index < 3; ++Index)
	{
		Squares.Add(AddNode(EWeaponTalentNodeShape::Square, FVector2D(Center + (Index - 1) * 180.0f, 382.0f), {HubA}, 1));
	}
	const int32 HubB = AddNode(EWeaponTalentNodeShape::Hub, FVector2D(Center, 478.0f), Squares);

	TArray<int32> FourB;
	for (int32 Index = 0; Index < 4; ++Index)
	{
		FourB.Add(AddNode(EWeaponTalentNodeShape::Circle, FVector2D(Center + (Index - 1.5f) * 150.0f, 574.0f), {HubB}));
	}
	const int32 HubC = AddNode(EWeaponTalentNodeShape::Hub, FVector2D(Center, 670.0f), FourB);

	for (int32 Index = 0; Index < 3; ++Index)
	{
		const float X = Center + (Index - 1) * 360.0f;
		const int32 Big = AddNode(EWeaponTalentNodeShape::Pentagon, FVector2D(X, 748.0f), {HubC}, 2);
		AddNode(EWeaponTalentNodeShape::SmallPentagon, FVector2D(X - 92.0f, 828.0f), {Big});
		AddNode(EWeaponTalentNodeShape::SmallPentagon, FVector2D(X + 92.0f, 828.0f), {Big});
		const int32 ChainA = AddNode(EWeaponTalentNodeShape::Circle, FVector2D(X, 828.0f), {Big});
		AddNode(EWeaponTalentNodeShape::Circle, FVector2D(X, 894.0f), {ChainA});
	}
}

void UWeaponTalentTreeWidget::AddMutexHintLines()
{
	TMap<int32, TArray<int32>> MutexGroups;
	for (const TPair<int32, FWeaponTalentNode>& Pair : Nodes)
	{
		if (Pair.Value.MutexGroup > 0)
		{
			MutexGroups.FindOrAdd(Pair.Value.MutexGroup).Add(Pair.Key);
		}
	}

	for (TPair<int32, TArray<int32>>& Group : MutexGroups)
	{
		TArray<int32>& NodeIds = Group.Value;
		if (NodeIds.Num() < 2)
		{
			continue;
		}

		// 互斥组同层横向排布，按 X 排序后只连相邻两个，避免出现交叉长线。
		NodeIds.Sort([this](const int32& A, const int32& B)
		{
			return Nodes.FindChecked(A).Position.X < Nodes.FindChecked(B).Position.X;
		});

		for (int32 i = 0; i < NodeIds.Num() - 1; ++i)
		{
			AddConnection(NodeIds[i], NodeIds[i + 1], true);
		}
	}
}

float UWeaponTalentTreeWidget::GetNodeSize(EWeaponTalentNodeShape Shape) const
{
	switch (Shape)
	{
	case EWeaponTalentNodeShape::Hub: return 62.0f;
	case EWeaponTalentNodeShape::Square: return 54.0f;
	case EWeaponTalentNodeShape::Pentagon: return 70.0f;
	case EWeaponTalentNodeShape::SmallPentagon: return 38.0f;
	default: return 46.0f;
	}
}

void UWeaponTalentTreeWidget::CreateConnectionWidget(FWeaponTalentConnection& Connection)
{
	const FWeaponTalentNode* From = Nodes.Find(Connection.FromId);
	const FWeaponTalentNode* To = Nodes.Find(Connection.ToId);
	if (!From || !To)
	{
		return;
	}

	const FVector2D Delta = To->Position - From->Position;
	const float Length = Delta.Size();
	UImage* Line = WidgetTree->ConstructWidget<UImage>();
	const FLinearColor InitialColor = Connection.bMutexHint ? WeaponTalentStyle::MutexHint : WeaponTalentStyle::LineDim;
	Line->SetBrush(WeaponTalentStyle::MakeBrush(InitialColor, 2.0f));
	Line->SetRenderTransformPivot(FVector2D(0.0f, 0.5f));
	Line->SetRenderTransformAngle(FMath::RadiansToDegrees(FMath::Atan2(Delta.Y, Delta.X)));
	UCanvasPanelSlot* LineSlot = TreeCanvas->AddChildToCanvas(Line);
	LineSlot->SetPosition(From->Position);
	// 互斥提示线更细并压到最底层，避免和父子实线混淆。
	LineSlot->SetSize(FVector2D(Length, Connection.bMutexHint ? 2.0f : 5.0f));
	LineSlot->SetZOrder(Connection.bMutexHint ? -1 : 0);
	Connection.Image = Line;
}

void UWeaponTalentTreeWidget::CreateNodeWidget(FWeaponTalentNode& Node)
{
	const float Size = GetNodeSize(Node.Shape);
	UWeaponTalentNodeButton* Button = WidgetTree->ConstructWidget<UWeaponTalentNodeButton>();
	Button->InitializeNode(Node.Id, this);
	Button->SetToolTipText(GetPlaceholderTooltip(Node.Id));
	if (Node.Shape == EWeaponTalentNodeShape::Pentagon || Node.Shape == EWeaponTalentNodeShape::SmallPentagon)
	{
		Button->SetRenderTransformAngle(45.0f);
	}
	UCanvasPanelSlot* NodeSlot = TreeCanvas->AddChildToCanvas(Button);
	NodeSlot->SetPosition(Node.Position - FVector2D(Size * 0.5f, Size * 0.5f));
	NodeSlot->SetSize(FVector2D(Size, Size));
	NodeSlot->SetZOrder(2);
	Node.Button = Button;
}

bool UWeaponTalentTreeWidget::AreParentsActivated(const FWeaponTalentNode& Node) const
{
	if (Node.Parents.IsEmpty())
	{
		return true;
	}

	// 三个收束节点只需任意一条相连路径被激活即可解锁。
	if (Node.Id == 6 || Node.Id == 10 || Node.Id == 15)
	{
		for (int32 ParentId : Node.Parents)
		{
			if (const FWeaponTalentNode* Parent = Nodes.Find(ParentId); Parent && Parent->bActivated)
			{
				return true;
			}
		}
		return false;
	}

	bool bMutexParents = Node.Parents.Num() > 1;
	int32 ParentGroup = 0;
	for (int32 ParentId : Node.Parents)
	{
		const FWeaponTalentNode* Parent = Nodes.Find(ParentId);
		if (!Parent)
		{
			return false;
		}
		if (ParentGroup == 0)
		{
			ParentGroup = Parent->MutexGroup;
		}
		bMutexParents &= ParentGroup != 0 && Parent->MutexGroup == ParentGroup;
	}

	for (int32 ParentId : Node.Parents)
	{
		const bool bActive = Nodes.FindChecked(ParentId).bActivated;
		if (bMutexParents && bActive)
		{
			return true;
		}
		if (!bMutexParents && !bActive)
		{
			return false;
		}
	}
	return !bMutexParents;
}

bool UWeaponTalentTreeWidget::IsMutexExcluded(const FWeaponTalentNode& Node) const
{
	if (Node.MutexGroup == 0 || Node.bActivated)
	{
		return false;
	}
	for (const TPair<int32, FWeaponTalentNode>& Pair : Nodes)
	{
		if (Pair.Value.MutexGroup == Node.MutexGroup && Pair.Value.bActivated)
		{
			return true;
		}
	}
	return false;
}

bool UWeaponTalentTreeWidget::CanActivate(const FWeaponTalentNode& Node) const
{
	return !Node.bActivated && AvailableTalentPoints > 0 && !IsMutexExcluded(Node) && AreParentsActivated(Node);
}

void UWeaponTalentTreeWidget::HandleNodeClicked(int32 NodeId)
{
	FWeaponTalentNode* Node = Nodes.Find(NodeId);
	if (!Node || !CanActivate(*Node))
	{
		return;
	}
	Node->bActivated = true;
	--AvailableTalentPoints;
	RefreshVisuals();
}

void UWeaponTalentTreeWidget::RefreshVisuals()
{
	using namespace WeaponTalentStyle;
	if (TalentPointsText)
	{
		TalentPointsText->SetText(FText::Format(NSLOCTEXT("WeaponTalent", "Points", "天赋点 {0}"), FText::AsNumber(AvailableTalentPoints)));
	}

	for (TPair<int32, FWeaponTalentNode>& Pair : Nodes)
	{
		FWeaponTalentNode& Node = Pair.Value;
		if (!Node.Button)
		{
			continue;
		}
		const float Radius = Node.Shape == EWeaponTalentNodeShape::Circle ? GetNodeSize(Node.Shape) * 0.5f : (Node.Shape == EWeaponTalentNodeShape::Hub ? 10.0f : 2.0f);
		const bool bInteractive = CanActivate(Node);
		const FLinearColor Color = Node.bActivated ? Amber : (bInteractive ? Available : Locked);
		Node.Button->SetStyle(MakeButtonStyle(Color, Radius, bInteractive));

		// 不用 SetIsEnabled(false)：disabled 控件不参与 hit-test，Tooltip 就弹不出来。
		// 锁定/互斥节点保持可命中以显示占位说明，点击由 HandleNodeClicked 内的 CanActivate 拦截；
		// 悬浮态在 MakeButtonStyle 里已改成极弱的灰度提亮，不会误导成可点击。
		Node.Button->SetIsEnabled(true);
		Node.Button->SetCursor(bInteractive ? EMouseCursor::Hand : EMouseCursor::Default);
	}

	for (FWeaponTalentConnection& Connection : Connections)
	{
		if (Connection.Image)
		{
			if (Connection.bMutexHint)
			{
				Connection.Image->SetColorAndOpacity(MutexHint);
			}
			else
			{
				const bool bBright = Nodes.FindChecked(Connection.FromId).bActivated && Nodes.FindChecked(Connection.ToId).bActivated;
				Connection.Image->SetColorAndOpacity(bBright ? Amber : LineDim);
			}
		}
	}
}

FText UWeaponTalentTreeWidget::GetPlaceholderTooltip(int32 NodeId) const
{
	return FText::Format(NSLOCTEXT("WeaponTalent", "PlaceholderTooltip", "天赋效果占位（节点 {0}）"), FText::AsNumber(NodeId));
}

void UWeaponTalentTreeWidget::HandleCloseClicked()
{
	CloseTalentTree();
}
