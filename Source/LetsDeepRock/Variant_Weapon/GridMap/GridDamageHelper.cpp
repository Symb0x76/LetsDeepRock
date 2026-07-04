// 网格地图系统 - 地形伤害接口实现

#include "GridDamageHelper.h"
#include "GridMapManager.h"
#include "WeaponGameplayTags.h"
#include "Kismet/GameplayStatics.h"
#include "EngineUtils.h"

bool FGridDamageHelper::TryApplyTerrainDamage(
	const UObject* WorldContext,
	const FVector& HitLocation,
	float DamageAmount,
	const FGameplayTagContainer& DamageTags)
{
	// 检查是否携带地形伤害 Tag
	if (!HasTerrainDamageTag(DamageTags))
	{
		return false;
	}

	// 查找地图管理器
	AGridMapManager* Manager = FindGridMapManager(WorldContext);
	if (!Manager)
	{
		return false;
	}

	// 对命中位置的格子施加伤害
	const int32 IntDamage = FMath::RoundToInt(DamageAmount);
	return Manager->DamageTileAtWorldLocation(HitLocation, IntDamage);
}

AGridMapManager* FGridDamageHelper::FindGridMapManager(const UObject* WorldContext)
{
	if (!WorldContext)
	{
		return nullptr;
	}

	UWorld* World = WorldContext->GetWorld();
	if (!World)
	{
		return nullptr;
	}

	// 遍历当前 World 的所有 AGridMapManager 实例（通常只有一个）
	for (TActorIterator<AGridMapManager> It(World); It; ++It)
	{
		return *It; // 返回第一个找到的
	}

	return nullptr;
}

bool FGridDamageHelper::HasTerrainDamageTag(const FGameplayTagContainer& DamageTags)
{
	return DamageTags.HasTag(TAG_Damage_Terrain);
}
