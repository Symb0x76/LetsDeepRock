// 网格地图系统 - 服务器权威管理器

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GridMapTypes.h"
#include "GridMapRoomAsset.h"
#include "GridMapManager.generated.h"

class UGridMapData;
class UGridMapGenerator;
class UGridMapRenderer;
class UVisionSourceComponent;

/**
 * 网格地图的唯一权威入口。
 * 服务器生成基础地图并提交所有运行时修改；客户端只重建基础地图和应用 Fast Array Delta。
 */
UCLASS(BlueprintType)
class LETSDEEPROCK_API AGridMapManager : public AActor
{
	GENERATED_BODY()

public:
	AGridMapManager();

	/** 关卡设计阶段使用的基础地图配置。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Config")
	FGridMapConfig MapConfig;

	/** 关卡设计阶段选择的确定性生成器。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Config")
	TSubclassOf<UGridMapGenerator> GeneratorClass;

	/** 0 表示服务器生成一个非零随机种子；所有生成器统一使用最终种子。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Config")
	int32 RequestedSeed = 0;

	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Config")
	bool bAutoGenerateOnBeginPlay = true;

	/** 建造意图允许的最大服务器校验距离。 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "GridMap|Validation", meta = (ClampMin = "0.0"))
	float MaxBuildDistance = 300.0f;

	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category = "GridMap|Components")
	TObjectPtr<UGridMapRenderer> MapRenderer;

	/** 仅服务器生成新的基础地图世代。 */
	UFUNCTION(BlueprintCallable, Category = "GridMap")
	void GenerateMap();

	UFUNCTION(BlueprintCallable, Category = "GridMap")
	UGridMapData* GetMapData() const { return MapData; }

	/** 服务器查询最近一次生成/恢复的 Recipe 中指定类型 marker 的世界坐标；客户端 Recipe 未复制，返回 false。 */
	UFUNCTION(BlueprintCallable, Category = "GridMap")
	bool GetMarkerWorldLocations(EGridMapMarkerType MarkerType, TArray<FVector>& OutLocations) const;

	/** 服务器查询指定用途房间的世界边界(2D);与 GetMarkerWorldLocations 同源(Recipe)。 */
	UFUNCTION(BlueprintCallable, Category = "GridMap")
	bool GetRoomWorldBoundsByPurpose(EGridMapRoomPurpose RoomPurpose, TArray<FBox2D>& OutBounds) const;

	UFUNCTION(BlueprintCallable, Category = "GridMap")
	bool GetTileAtWorldLocation(const FVector& WorldLocation, FGridTileData& OutTileData) const;

	/** 仅服务器内部调用；伤害来源必须已经通过 Ability/命中验证。 */
	bool DamageTile(int32 GridX, int32 GridY, int32 DamageAmount);
	bool DamageTileAtWorldLocation(const FVector& WorldLocation, int32 DamageAmount);

	/** 仅服务器对圆形范围内格子施加线性距离衰减伤害。 */
	bool DamageTilesInRadius(const FVector& Center, float Radius, float MaxDamage);

	/** 仅服务器对轴对齐矩形范围内格子施加无衰减伤害。 */
	bool DamageTilesInRect(const FVector& Center, float HalfWidth, float HalfHeight, float MaxDamage);

	/** 服务器验证玩家距离、格子状态和建造类型后提交建造 Delta。 */
	bool TryBuildTileAtWorldLocation(AActor* RequestingActor, const FVector& WorldLocation, EGridTileType BuildType);

	/** 仅 Authority 导出/恢复 GridMap 的确定性基线与稀疏变化。 */
	bool CaptureRecoveryState(FGridMapRecoveryState& OutState, FString& OutError) const;
	static bool ValidateRecoveryState(const FGridMapRecoveryState& State, FString& OutError);
	bool RestoreRecoveryState(const FGridMapRecoveryState& State, FString& OutError);

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

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(FOnTileDestroyed, int32, GridX, int32, GridY);

	UPROPERTY(BlueprintAssignable, Category = "GridMap|Events")
	FOnTileDestroyed OnTileDestroyed;

	DECLARE_DYNAMIC_MULTICAST_DELEGATE_ThreeParams(FOnTileChanged, int32, GridX, int32, GridY, EGridTileType, TileType);

	UPROPERTY(BlueprintAssignable, Category = "GridMap|Events")
	FOnTileChanged OnTileChanged;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** Fast Array 回调入口；仅用于客户端应用服务器 Delta。 */
	void HandleReplicatedTileDelta(const FGridTileDeltaItem& Delta);

protected:
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;

private:
	UPROPERTY()
	TObjectPtr<UGridMapData> MapData;

	UPROPERTY()
	TObjectPtr<UGridMapGenerator> Generator;

	UPROPERTY(ReplicatedUsing = OnRep_BaselineState)
	FGridMapBaselineState BaselineState;

	UPROPERTY(Replicated)
	FGridTileDeltaArray TileDeltas;

	uint32 NextTileRevision = 0;

	UFUNCTION()
	void OnRep_BaselineState();

	/** 根据版本化描述在当前进程重建确定性基础地图。 */
	bool BuildBaselineMap(const FGridMapBaselineState& State, FString* OutError = nullptr);

	/** 服务器提交单格最新状态并标记 Fast Array ItemDirty。 */
	void CommitTileState(int32 GridX, int32 GridY, EGridTileType TileType, int32 Health, int32 MaxHealth);

	/** 将单个状态写入数据层，并局部更新渲染和 NavMesh。 */
	void ApplyTileState(const FGridTileDeltaItem& Delta);

	void ApplyAllReplicatedDeltas();
	void MarkTileNavigationDirty(int32 GridX, int32 GridY);
	void FlushNavigationDirtyChunks();
	int32 GetDefaultHealthForType(EGridTileType TileType) const;
	FGridNavigationDirtyTracker NavigationDirtyTracker;

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

};
