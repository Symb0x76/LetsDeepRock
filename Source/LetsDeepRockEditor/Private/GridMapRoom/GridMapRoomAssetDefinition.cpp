// 网格房间模板资产 - Content Browser 集成

#include "GridMapRoomAssetDefinition.h"

#include "AssetTypeCategories.h"
#include "GridMap/GridMapRoomAsset.h"
#include "GridMapRoomEditorToolkit.h"

#define LOCTEXT_NAMESPACE "GridMapRoomAssetDefinition"

// ---------------------------------------------------------------------------
// AssetDefinition
// ---------------------------------------------------------------------------

FText UAssetDefinition_GridMapRoom::GetAssetDisplayName() const
{
	return LOCTEXT("AssetDisplayName", "网格房间模板");
}

FLinearColor UAssetDefinition_GridMapRoom::GetAssetColor() const
{
	return FLinearColor(FColor(58, 168, 176));
}

TSoftClassPtr<UObject> UAssetDefinition_GridMapRoom::GetAssetClass() const
{
	return UGridMapRoomAsset::StaticClass();
}

TConstArrayView<FAssetCategoryPath> UAssetDefinition_GridMapRoom::GetAssetCategories() const
{
	static const auto Categories = { EAssetCategoryPaths::Gameplay };
	return Categories;
}

EAssetCommandResult UAssetDefinition_GridMapRoom::OpenAssets(const FAssetOpenArgs& OpenArgs) const
{
	const EToolkitMode::Type Mode = OpenArgs.GetToolkitMode();

	for (UGridMapRoomAsset* Room : OpenArgs.LoadObjects<UGridMapRoomAsset>())
	{
		if (Room)
		{
			const TSharedRef<FGridMapRoomEditorToolkit> Toolkit = MakeShared<FGridMapRoomEditorToolkit>();
			Toolkit->InitEditor(Mode, OpenArgs.ToolkitHost, Room);
		}
	}

	return EAssetCommandResult::Handled;
}

// ---------------------------------------------------------------------------
// Factory
// ---------------------------------------------------------------------------

UGridMapRoomAssetFactory::UGridMapRoomAssetFactory()
{
	SupportedClass = UGridMapRoomAsset::StaticClass();
	bCreateNew = true;
	bEditAfterNew = true;
}

UObject* UGridMapRoomAssetFactory::FactoryCreateNew(UClass* InClass, UObject* InParent, const FName InName,
	const EObjectFlags Flags, UObject* Context, FFeedbackContext* Warn)
{
	UGridMapRoomAsset* NewRoom = NewObject<UGridMapRoomAsset>(InParent, InClass, InName, Flags);

	// 默认给一个符合连接房尺寸下限的 9x9 空房，开箱即可编辑
	NewRoom->InitializeRoom(9, 9, EGridTileType::Empty);
	return NewRoom;
}

FText UGridMapRoomAssetFactory::GetDisplayName() const
{
	return LOCTEXT("FactoryDisplayName", "网格房间模板");
}

uint32 UGridMapRoomAssetFactory::GetMenuCategories() const
{
	return EAssetTypeCategories::Gameplay;
}

#undef LOCTEXT_NAMESPACE
