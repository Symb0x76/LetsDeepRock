// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GrenadeBallistics.h"
#include "GrenadeTrajectoryPreviewComponent.generated.h"

class UInstancedStaticMeshComponent;
class UStaticMesh;
class UWeaponGrenadeDataAsset;

/**
 * 手雷弹道轨迹预览组件
 *
 * 非复制、仅本地控制 Pawn 工作的纯客户端组件。
 * 使用 UInstancedStaticMeshComponent + 引擎内置 Sphere 网格，
 * 沿 PredictProjectilePath 的 PathData 显示轨迹点，
 * 并在预测爆炸点绘制爆炸范围轮廓。
 *
 * 支持 Sphere/Capsule（XY 投影圆）和 Box（矩形）的爆炸形状预览。
 * 不每帧 Spawn Actor，不使用 DrawDebug 作为正式实现。
 *
 * 生命周期：由 AWeaponCharacter 在 BeginPlay 中创建，EndPlay/死亡时清理。
 */
UCLASS(ClassGroup = (Weapon), meta = (BlueprintSpawnableComponent))
class UGrenadeTrajectoryPreviewComponent : public UActorComponent
{
	GENERATED_BODY()

public:

	UGrenadeTrajectoryPreviewComponent();

	//~ UActorComponent
	virtual void InitializeComponent() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 使用解算结果显示预览 */
	void ShowPreview(const FGrenadeBallisticsOutput& BallisticsResult, const UWeaponGrenadeDataAsset* Data);

	/** 更新预览（重算弹道后调用，复用同一 ISM） */
	void UpdatePreview(const FGrenadeBallisticsOutput& BallisticsResult, const UWeaponGrenadeDataAsset* Data);

	/** 隐藏预览（Completed/Canceled 时调用） */
	void HidePreview();

	/** 是否当前显示预览 */
	bool IsPreviewVisible() const { return bPreviewVisible; }

	/** 设置预览标记缩放（可在蓝图中调整） */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preview")
	float MarkerScale = 0.25f;

	/** 轨迹点间距（cm），控制路径上点的稀疏度 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preview")
	float PointSpacing = 50.0f;

	/** 爆炸范围轮廓点数量 */
	UPROPERTY(EditAnywhere, BlueprintReadWrite, Category = "Preview")
	int32 RangePointCount = 48;

private:

	/** 清理所有 ISM 实例 */
	void ClearAllInstances();

	/** 放置轨迹路径点（沿 PathPoints） */
	void PlaceTrajectoryPoints(const TArray<FVector>& PathPoints);

	/** 放置爆炸范围轮廓（圆形） */
	void PlaceExplosionCircle(const FVector& Center, float Radius);

	/** 放置爆炸范围轮廓（矩形） */
	void PlaceExplosionRect(const FVector& Center, const FVector2D& HalfExtents);

	/** 轨迹点 ISM（沿弹道路径的小球） */
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> TrajectoryISM;

	/** 爆炸范围 ISM（在地面绘制轮廓） */
	UPROPERTY()
	TObjectPtr<UInstancedStaticMeshComponent> ExplosionPreviewISM;

	/** 引擎内置 Sphere 静态网格（运行时加载，缺失时无崩溃） */
	UPROPERTY()
	TObjectPtr<UStaticMesh> SphereMesh;

	/** 是否当前可见 */
	bool bPreviewVisible = false;
};
