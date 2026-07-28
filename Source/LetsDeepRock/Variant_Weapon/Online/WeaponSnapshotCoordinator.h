#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "WeaponSnapshotCoordinator.generated.h"

class UAbilitySystemComponent;
struct FGameplayTag;
struct FWeaponSnapshot;

/** 服务器权威快照入口；负责捕获一致性边界和恢复前校验 */
UCLASS()
class LETSDEEPROCK_API UWeaponSnapshotCoordinator : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** 只允许服务器从当前 World 捕获已有 owner 的最小战局状态 */
	bool CaptureSnapshot(const UWorld* World, FWeaponSnapshot& OutSnapshot, FString& OutError);

	/** 在 Restore 前检查版本、身份、槽位和数值有效性 */
	bool ValidateSnapshot(const FWeaponSnapshot& Snapshot, FString& OutError);
	
	/** 恢复快照到当前战局状态 */
	bool RestoreSnapshot(const FWeaponSnapshot& Snapshot, FString& OutError);

private:
	bool CapturePlayers(const UWorld* World, FWeaponSnapshot& Snapshot, FString& OutError) const;
	bool CaptureEnemies(const UWorld* World, FWeaponSnapshot& Snapshot, FString& OutError);
	bool RestorePlayers(const FWeaponSnapshot& Snapshot, FString& OutError) const;
	bool RestoreEnemies(const FWeaponSnapshot& Snapshot, FString& OutError) const;
	static TMap<FGameplayTag, float> CaptureAttributeValues(const UAbilitySystemComponent* ASC);
	static void RestoreAttributeValues(UAbilitySystemComponent* ASC, const TMap<FGameplayTag, float>& Values);
	static bool IsFiniteVector(const FVector& Value);

	/** 当前服务器战局身份；跨地图保留，不能在每次 Capture 时重新生成 */
	FGuid ActiveMatchId;
	int64 NextSnapshotRevision = 1;
};
