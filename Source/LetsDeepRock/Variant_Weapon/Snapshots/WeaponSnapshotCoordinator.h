#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "WeaponSnapshotCoordinator.generated.h"

struct FWeaponMatchSnapshot;

/** 服务器权威快照入口；负责捕获一致性边界和恢复前校验。 */
UCLASS()
class LETSDEEPROCK_API UWeaponSnapshotCoordinator : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** 只允许服务器从当前 World 捕获已有 owner 的最小战局状态。 */
	bool CaptureSnapshot(UWorld* World, FWeaponMatchSnapshot& OutSnapshot, FString& OutError);

	/** 在 Restore 前检查版本、身份、槽位和数值有效性。 */
	bool ValidateSnapshot(const FWeaponMatchSnapshot& Snapshot, FString& OutError) const;

private:
	bool CapturePlayers(const UWorld* World, FWeaponMatchSnapshot& Snapshot, FString& OutError) const;
	bool CaptureNPCs(const UWorld* World, FWeaponMatchSnapshot& Snapshot, FString& OutError) const;
	static bool IsFiniteVector(const FVector& Value);

	/** 当前服务器战局身份；跨地图保留，不能在每次 Capture 时重新生成。 */
	FGuid ActiveMatchId;
	int64 NextSnapshotRevision = 1;
};
