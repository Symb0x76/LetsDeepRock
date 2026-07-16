// 网格地图系统 - 地形伤害接口

#pragma once

#include "CoreMinimal.h"
#include "GameplayTagContainer.h"

class AGridMapManager;

/**
 * FGridDamageHelper
 * 
 * 地形伤害辅助工具。提供静态函数，用于在武器命中检测中判断
 * 命中物是否为网格格子，并传递伤害。
 * 
 * 使用方式：
 * 1. 武器命中 Actor 后，先尝试标准 GAS 链路
 * 2. 如果目标无 ASC，调用 TryApplyTerrainDamage 尝试地形伤害
 * 3. 内部检查伤害是否携带 Damage.Terrain Tag
 */
class LETSDEEPROCK_API FGridDamageHelper
{
public:
	/**
	 * 尝试对命中位置施加地形伤害。
	 * 
	 * @param WorldContext    世界上下文对象（用于查找 GridMapManager）
	 * @param HitLocation    命中世界位置
	 * @param DamageAmount   伤害值
	 * @param DamageTags     伤害携带的 GameplayTag 容器
	 * @return true 如果成功对格子施加了伤害
	 */
	static bool TryApplyTerrainDamage(
		const UObject* WorldContext,
		const FVector& HitLocation,
		float DamageAmount,
		const FGameplayTagContainer& DamageTags);

	/**
	 * 查找当前关卡中的 GridMapManager。
	 * 缓存结果以避免重复查找。
	 */
	static AGridMapManager* FindGridMapManager(const UObject* WorldContext);

	/**
	 * 检查伤害 Tag 容器是否包含地形伤害标签（Damage.Terrain）。
	 */
	static bool HasTerrainDamageTag(const FGameplayTagContainer& DamageTags);

	/**
	 * 尝试对圆形范围施加地形伤害（距离线性衰减）。
	 *
	 * @param WorldContext    世界上下文对象（用于查找 GridMapManager）
	 * @param Center          爆炸中心世界位置
	 * @param Radius          伤害半径（cm）
	 * @param MaxDamage       中心最大伤害值（边界衰减到 0）
	 * @param DamageTags      伤害携带的 GameplayTag 容器（必须含 Damage.Terrain）
	 * @return true 如果至少对一个格子施加了伤害
	 */
	static bool TryApplyCircularTerrainDamage(
		const UObject* WorldContext,
		const FVector& Center,
		float Radius,
		float MaxDamage,
		const FGameplayTagContainer& DamageTags);

	/**
	 * 尝试对矩形范围施加地形伤害（无距离衰减，区域内全伤害）。
	 * 矩形沿世界 X 轴和 Y 轴对齐。
	 *
	 * @param WorldContext    世界上下文对象（用于查找 GridMapManager）
	 * @param Center          矩形中心世界位置
	 * @param HalfWidth       矩形半宽（世界 X 轴方向，cm）
	 * @param HalfHeight      矩形半高（世界 Y 轴方向，cm）
	 * @param MaxDamage       区域内每个格子的伤害值
	 * @param DamageTags      伤害携带的 GameplayTag 容器（必须含 Damage.Terrain）
	 * @return true 如果至少对一个格子施加了伤害
	 */
	static bool TryApplyRectTerrainDamage(
		const UObject* WorldContext,
		const FVector& Center,
		float HalfWidth,
		float HalfHeight,
		float MaxDamage,
		const FGameplayTagContainer& DamageTags);
};
