// 网格地图系统 - 管理器

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GridMapTypes.h"
#include "GridMapManager.generated.h"

class UGridMapData;
class UGridMapGenerator;
class UGridMapRenderer;
class UVisionSourceComponent;

/**
 * AGridMapManager
 * 
 * 网格地图总管理器。放置在关卡中，负责：
 * 1. 持有地图数据 (UGridMapData)
 * 2. 调用生成器填充地图
 * 3. 驱动渲染层（HISM 实例化显示）
 * 4. 提供对外接口查询/修改格子状态
 * 5. 处理格子破坏后的渲染/碰撞/NavMesh 更新
 */
UCLASS(BlueprintType)
class LETSDEEPROCK_API AGridMapManager : public AActor
{
	GENERATED_BODY()

public:
	AGridMapManager();

	// ===== 配置 =====

	/** 地图配置 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Config")
	FGridMapConfig MapConfig;

	/** 生成器类（在编辑器中选择要使用的生成器） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Config")
	TSubclassOf<UGridMapGenerator> GeneratorClass;

	/** 是否在 BeginPlay 时自动生成地图 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Config")
	bool bAutoGenerateOnBeginPlay = true;

	// ===== 组件 =====

	/** 渲染器组件 */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GridMap|Components")
	TObjectPtr<UGridMapRenderer> MapRenderer;

	// ===== 运行时接口 =====

	/** 生成地图（初始化数据 + 调用生成器 + 触发渲染） */
	UFUNCTION(BlueprintCallable, Category = "GridMap")
	void GenerateMap();

	/** 获取地图数据 */
	UFUNCTION(BlueprintCallable, Category = "GridMap")
	UGridMapData* GetMapData() const { return MapData; }

	/** 获取指定世界位置对应的格子数据 */
	UFUNCTION(BlueprintCallable, Category = "GridMap")
	bool GetTileAtWorldLocation(const FVector& WorldLocation, FGridTileData& OutTileData) const;

	/** 对指定格子施加伤害（返回是否被破坏） */
	UFUNCTION(BlueprintCallable, Category = "GridMap")
	bool DamageTile(int32 GridX, int32 GridY, int32 DamageAmount);

	/** 对指定世界位置的格子施加伤害（返回是否被破坏） */
	UFUNCTION(BlueprintCallable, Category = "GridMap")
	bool DamageTileAtWorldLocation(const FVector& WorldLocation, int32 DamageAmount);

	/**
	 * 对圆形范围内的所有格子施加距离衰减伤害。
	 *
	 * 遍历范围内所有格子，按距离线性衰减：
	 *   Damage = MaxDamage * (1 - Distance / Radius)
	 * 中心为 MaxDamage，边界为 0。
	 * 仅对有 Damage.Terrain 标签的伤害源生效（调用方已预先过滤）。
	 *
	 * @param Center      圆心世界位置
	 * @param Radius      伤害半径（cm）
	 * @param MaxDamage   中心最大伤害值
	 * @return true 如果至少对一个格子造成了伤害
	 */
	bool DamageTilesInRadius(const FVector& Center, float Radius, float MaxDamage);

	/**
	 * 对轴对齐矩形范围内的所有格子施加伤害（无距离衰减）。
	 * 矩形沿世界 X 轴（HalfWidth）和 Y 轴（HalfHeight）对齐。
	 *
	 * @param Center      矩形中心世界位置
	 * @param HalfWidth   世界 X 轴方向半宽（cm）
	 * @param HalfHeight  世界 Y 轴方向半高（cm）
	 * @param MaxDamage   区域内每个格子的伤害值
	 * @return true 如果至少对一个格子造成了伤害
	 */
	bool DamageTilesInRect(const FVector& Center, float HalfWidth, float HalfHeight, float MaxDamage);

	/** 查询本地玩家队伍当前是否可见指定格子。 */
	UFUNCTION(BlueprintPure, Category = "GridMap|Vision")
	bool IsTileVisible(int32 GridX, int32 GridY) const;

	UFUNCTION(BlueprintPure, Category = "GridMap|Vision")
	int32 GetVisibleTileCount() const;

	/** 地形或外部状态变化后请求下一帧重新计算视野。 */
	UFUNCTION(BlueprintCallable, Category = "GridMap|Vision")
	void MarkVisionDirty();

	/** 是否绘制本地视野调试图形。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Vision|Debug")
	bool bDrawVisionDebug = false;

	/** 绿色格直接来自实际视野使用的同一 bitset；单次绘制可能被此上限截断。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Vision|Debug", meta = (ClampMin = "0"))
	int32 MaxDebugDrawTiles = 2000;

	// ===== 事件 =====

	/** 格子被破坏时广播 */
	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnTileDestroyed, int32, GridX, int32, GridY);

	UPROPERTY(BlueprintAssignable, Category = "GridMap|Events")
	FOnTileDestroyed OnTileDestroyed;

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	/** 地图数据实例 */
	UPROPERTY()
	TObjectPtr<UGridMapData> MapData;

	/** 生成器实例 */
	UPROPERTY()
	TObjectPtr<UGridMapGenerator> Generator;

	/** 仅本地维护的当前队伍可见位图，不参与复制。 */
	TBitArray<> VisibleTiles;

	float VisionUpdateAccumulator = 0.0f;
	bool bVisionDirty = true;
	static constexpr float VisionUpdateInterval = 0.15f;
	static constexpr float VisionDirectionChangeDegrees = 2.0f;

	struct FVisionSourceSnapshot
	{
		TWeakObjectPtr<UVisionSourceComponent> Source;
		uint8 Shape = 0;
		float Range = 0.0f;
		float HalfAngle = 0.0f;
		int32 TeamId = 0;
		int32 GridX = INDEX_NONE;
		int32 GridY = INDEX_NONE;
		FVector2D Direction = FVector2D::ZeroVector;
	};

	TArray<FVisionSourceSnapshot> VisionSourceSnapshots;
	int32 LastSampledLocalTeamId = 0;
	bool bHasSampledLocalTeam = false;

	void SampleVisionSourceState();
	void RebuildLocalVisibility();
	bool HasLineOfSight(int32 SourceX, int32 SourceY, int32 TargetX, int32 TargetY) const;
	void DrawVisionDebug() const;

	/** 内部：破坏格子后的清理工作（移除渲染实例、更新 NavMesh） */
	void OnTileDestroyedInternal(int32 GridX, int32 GridY);
};
