// 网格地图阶段三 - 房间图生成器

#pragma once

#include "CoreMinimal.h"
#include "GridMapGenerator.h"
#include "GridMapRoomAsset.h"
#include "GridMapGenerator_RoomGraph.generated.h"

UCLASS(BlueprintType, Blueprintable)
class LETSDEEPROCK_API UGridMapGenerator_RoomGraph : public UGridMapGenerator
{
	GENERATED_BODY()

public:
	static constexpr int32 CurrentAlgorithmVersion = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoomGraph")
	TArray<TSoftObjectPtr<UGridMapRoomAsset>> RoomTemplates;

	/** 仅供不依赖磁盘资产的纯数据 Automation 使用。 */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UGridMapRoomAsset>> TransientTemplates;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoomGraph", meta = (ClampMin = "1"))
	int32 AlgorithmVersion = CurrentAlgorithmVersion;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoomGraph", meta = (ClampMin = "1"))
	int32 TemplateSetVersion = 1;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoomGraph", meta = (ClampMin = "1", ClampMax = "8"))
	int32 ConnectorRoomCount = 2;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoomGraph", meta = (ClampMin = "1", ClampMax = "20"))
	int32 WholeMapRetries = 4;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "RoomGraph", meta = (ClampMin = "1", ClampMax = "5"))
	int32 CorridorWidth = 1;

	virtual bool GenerateChecked(UGridMapData* MapData) override;
	virtual void Generate(UGridMapData* MapData) override { GenerateChecked(MapData); }
	virtual int32 GetAlgorithmVersion() const override { return AlgorithmVersion; }
	virtual int32 GetTemplateSetVersion() const override { return TemplateSetVersion; }

	const TArray<FIntRect>& GetLastRoomBounds() const { return LastRoomBounds; }
	bool IsLastTopologyReachable() const { return bLastTopologyReachable; }
	static uint64 ComputeTemplateContentHash(const UGridMapRoomAsset& Room);

private:
	TArray<FIntRect> LastRoomBounds;
	bool bLastTopologyReachable = false;
	bool BuildNewRecipe(UGridMapData* MapData, FRandomStream& Random);
	bool Replay(UGridMapData* MapData, const FGridMapGenerationRecipe& Recipe);
};
