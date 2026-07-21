#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "WeaponMatchSnapshot.h"
#include "WeaponSnapshotCoordinator.generated.h"

/** 服务器权威快照入口；负责捕获一致性边界和恢复前校验。 */
UCLASS()
class LETSDEEPROCK_API UWeaponSnapshotCoordinator : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;

	/** 只允许服务器从当前 World 捕获根战局和玩家基础状态。 */
	UFUNCTION(BlueprintCallable, Category = "Snapshot")
	bool CaptureSnapshot(UWorld* World, FWeaponMatchSnapshot& OutSnapshot, FString& OutError);

	/** 在 Restore 前检查身份、时间、槽位、引用和 GridMap 版本。 */
	UFUNCTION(BlueprintCallable, Category = "Snapshot")
	bool ValidateSnapshot(const FWeaponMatchSnapshot& Snapshot, FString& OutError) const;

private:
	bool CapturePlayers(UWorld* World, FWeaponMatchSnapshot& Snapshot, FString& OutError) const;
	static bool IsFiniteVector(const FVector& Value);

	/** 当前服务器战局身份；跨地图保留，不能在每次 Capture 时重新生成。 */
	FGuid ActiveMatchId;
	mutable int64 NextSnapshotRevision = 1;
};
