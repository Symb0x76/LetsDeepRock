// 一次性原型(Spike):验证开放丘陵地形的手感。不进入正式玩法,随时可删除。

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "OpenTerrainSpike.generated.h"

class UProceduralMeshComponent;

/**
 *  种子高度场地形:服务器生成 Actor 并复制种子,各端按种子本地重建网格与碰撞。
 *  控制台:Spike.Terrain [Seed] 在本地玩家脚下生成(替换旧的);Spike.TerrainClear 移除。
 */
UCLASS(NotPlaceable)
class AOpenTerrainSpike : public AActor
{
	GENERATED_BODY()

public:
	AOpenTerrainSpike();

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;

	void SetSeedAuthority(int32 InSeed);

	/** 顶点数(每边)、顶点间距、最大起伏(uu) */
	static constexpr int32 VertsPerSide = 256;
	static constexpr float VertexSpacing = 100.f;
	static constexpr float MaxHeight = 1500.f;
	/** 出生点周围的平地半径(内:完全平坦;外:平滑过渡到丘陵) */
	static constexpr float FlatInnerRadius = 800.f;
	static constexpr float FlatOuterRadius = 2000.f;

	/** 纯函数:相对 Actor 原点的局部高度(便于以后迁成正式生成器时测试) */
	static float SampleHeight(int32 Seed, float LocalX, float LocalY);

private:
	UFUNCTION()
	void OnRep_Seed();

	void BuildTerrain();
	void ReleasePlaneConstraints();

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UProceduralMeshComponent> Mesh;

	UPROPERTY(ReplicatedUsing = OnRep_Seed)
	int32 Seed = 0;

	bool bBuilt = false;
	FTimerHandle PlaneConstraintTimer;
};
