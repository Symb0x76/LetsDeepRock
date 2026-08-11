// 网格房间模板资产 - Content Browser 集成

#pragma once

#include "CoreMinimal.h"
#include "AssetDefinitionDefault.h"
#include "Factories/Factory.h"

#include "GridMapRoomAssetDefinition.generated.h"

/** 在 Content Browser 中注册房间模板资产类型，并接管双击打开行为。 */
UCLASS()
class UAssetDefinition_GridMapRoom : public UAssetDefinitionDefault
{
	GENERATED_BODY()

public:
	virtual FText GetAssetDisplayName() const override;
	virtual FLinearColor GetAssetColor() const override;
	virtual TSoftClassPtr<UObject> GetAssetClass() const override;
	virtual TConstArrayView<FAssetCategoryPath> GetAssetCategories() const override;
	virtual EAssetCommandResult OpenAssets(const FAssetOpenArgs& OpenArgs) const override;
};

/** 右键 → Gameplay → 房间模板 创建资产。 */
UCLASS()
class UGridMapRoomAssetFactory : public UFactory
{
	GENERATED_BODY()

public:
	UGridMapRoomAssetFactory();

	virtual UObject* FactoryCreateNew(UClass* InClass, UObject* InParent, FName InName, EObjectFlags Flags,
		UObject* Context, FFeedbackContext* Warn) override;
	virtual FText GetDisplayName() const override;
	virtual uint32 GetMenuCategories() const override;
};
