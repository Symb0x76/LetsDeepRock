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
};
