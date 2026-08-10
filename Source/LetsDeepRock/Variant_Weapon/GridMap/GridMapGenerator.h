// 网格地图系统 - 生成器

#pragma once

#include "CoreMinimal.h"
#include "GridMapTypes.h"
#include "GridMapGenerator.generated.h"

class UGridMapData;

/**
 * UGridMapGenerator
 * 
 * 地图生成器基类。负责根据配置填充 GridMapData 中的格子类型。
 * 
 * 子类可实现不同的生成算法（噪声、房间走廊、随机散布等）。
 * 当前提供一个默认实现：边界岩石 + 内部空地。
 */
UCLASS(BlueprintType, Blueprintable, Abstract)
class LETSDEEPROCK_API UGridMapGenerator : public UObject
{
	GENERATED_BODY()

public:
	/** Manager 统一提供的服务器有效种子；生成器不得自行换种子。 */
	void SetEffectiveSeed(int32 InSeed) { EffectiveSeed = InSeed; }
	int32 GetEffectiveSeed() const { return EffectiveSeed; }

	virtual bool GenerateChecked(UGridMapData* MapData);
	virtual int32 GetAlgorithmVersion() const { return 0; }
	virtual int32 GetTemplateSetVersion() const { return 0; }
	virtual void SetReplayRecipe(const FGridMapGenerationRecipe& InRecipe) { ReplayRecipe = InRecipe; }
	virtual const FGridMapGenerationRecipe& GetGeneratedRecipe() const { return GeneratedRecipe; }
	const FString& GetLastError() const { return LastError; }

	static uint64 ComputeStableResultHash(const UGridMapData& MapData, int32 Seed,
		int32 AlgorithmVersion, int32 TemplateSetVersion, const FGridMapGenerationRecipe& Recipe);

	/**
	 * 执行地图生成，填充 MapData 中的格子数据。
	 * 子类必须重写此函数实现具体生成算法。
	 * 
	 * @param MapData 目标地图数据（已初始化为全空地）
	 */
	UFUNCTION(BlueprintCallable)
	virtual void Generate(UGridMapData* MapData);

protected:
	void WriteTile(UGridMapData* MapData, int32 X, int32 Y, EGridTileType Type) const;
	int32 EffectiveSeed = 1;
	FGridMapGenerationRecipe ReplayRecipe;
	FGridMapGenerationRecipe GeneratedRecipe;
	FString LastError;
};

/**
 * UGridMapGenerator_Default
 * 
 * 默认生成器：四周一圈岩石边界，内部全为空地。
 * 用于初期测试框架是否正常工作。
 */
UCLASS(BlueprintType, Blueprintable)
class LETSDEEPROCK_API UGridMapGenerator_Default : public UGridMapGenerator
{
	GENERATED_BODY()

public:
	virtual void Generate(UGridMapData* MapData) override;
};

/**
 * UGridMapGenerator_Simple
 * 
 * 简单测试生成器：四周岩石边界 + 内部随机散布少量树木。
 * 用于验证渲染和碰撞是否正常。
 */
UCLASS(BlueprintType, Blueprintable)
class LETSDEEPROCK_API UGridMapGenerator_Simple : public UGridMapGenerator
{
	GENERATED_BODY()

public:
	// 一般树木的生成概率（0~1）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float TreeDensity = 0.03f;

	// 高大树木的生成概率（0~1）
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (ClampMin = "0.0", ClampMax = "1.0"))
	float LargeTreeDensity = 0.01f;

	/** 旧资产迁移字段；仅服务器 Manager 在 RequestedSeed=0 时读取，生成器自身不使用。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, meta = (DeprecatedProperty, DeprecationMessage = "请改用 GridMapManager.RequestedSeed"))
	int32 Seed = 0;

	virtual void Generate(UGridMapData* MapData) override;
};

/**
 * UGridMapGenerator_Test
 * 
 * 测试生成器：在地图中央区域各放置一个 Tree、Rock、LargeTree。
 * 无边界围墙，用于单独测试各类型障碍物的阻挡和破坏效果。
 */
UCLASS(BlueprintType, Blueprintable)
class LETSDEEPROCK_API UGridMapGenerator_Test : public UGridMapGenerator
{
	GENERATED_BODY()

public:
	virtual void Generate(UGridMapData* MapData) override;
};
