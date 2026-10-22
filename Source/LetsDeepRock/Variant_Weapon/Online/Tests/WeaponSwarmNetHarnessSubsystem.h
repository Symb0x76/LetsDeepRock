// Swarm 多进程网络冒烟：命令行驱动的 GameInstance 子系统（不进入 Shipping）

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Online/WeaponSessionSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "WeaponSwarmNetHarnessSubsystem.generated.h"

class ASwarmBugPawn;

/**
 * Swarm 网络冒烟 harness（由 Tools/RunSwarmNetSmoke.ps1 驱动）。
 *
 * 仅当命令行带 -WeaponSwarmHarnessRole=Host|Client 时创建，Shipping 永不创建。
 * Host：LAN 主机于测试地图 → 等待玩家数达标后生成 30 只 BasicGrunt（SPAWNED）→
 *       延时后 DespawnAll 并在 ≥3000uu 外重新生成（RECYCLED）→ 每秒上报存活数与平均位置。
 * Client：LAN 搜索并加入 → 每秒上报可见激活虫数、平均位置、入站 KB/s 与"滑行"计数。
 * 所有输出为 WSH|Run=..|Process=..|Event=..|k=v 标记行，脚本据此断言。
 */
UCLASS()
class UWeaponSwarmNetHarnessSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	enum class EProcessRole : uint8
	{
		None,
		Host,
		Client,
	};

	/** 客户端逐帧滑行跟踪（每个池化虫一份） */
	struct FBugTrack
	{
		FVector LastLocation = FVector::ZeroVector;
		int32 ConsecutiveFastFrames = 0;
		bool bWasActive = false;
	};

	UFUNCTION()
	void HandleHostComplete(bool bWasSuccessful, FString Message);

	UFUNCTION()
	void HandleFindComplete(bool bWasSuccessful, const TArray<FWeaponLanSessionResult>& Results);

	UFUNCTION()
	void HandleJoinComplete(bool bWasSuccessful, FString Message);

	bool TickHarness(float DeltaSeconds);
	void DriveSession(UWorld& World);
	void TickHost(UWorld& World);
	void TickClient(UWorld& World);
	void MakePlayersInvulnerable(UWorld& World) const;
	bool SpawnGruntsNear(UWorld& World, const FVector& Origin, float MinDistance, float MaxDistance, int32& OutSpawned, FVector& OutCenter) const;
	void EmitHostSample(UWorld& World, int64 Second);
	void EmitClientSample(UWorld& World, int64 Second, double NowSeconds);
	void EmitMarker(const TCHAR* Event, const FString& Details = FString()) const;

	static FString SanitizeMarkerValue(FString Value);

	EProcessRole ProcessRole = EProcessRole::None;
	FString ProcessLabel;
	FString RunId;
	FString ServerName;
	FName HostMapName;
	FTSTicker::FDelegateHandle TickerHandle;

	// 会话驱动
	double NextFindAttemptSeconds = 0.0;
	bool bHostRequestStarted = false;
	bool bFindInFlight = false;
	bool bJoinStarted = false;

	// Host 时间线
	int32 MinPlayersToSpawn = 2;
	double RecycleDelaySeconds = 15.0;
	bool bHostReadyEmitted = false;
	double PlayersReadySeconds = 0.0;
	bool bSpawned = false;
	double SpawnedSeconds = 0.0;
	bool bRecycled = false;
	int32 LastPlayerCount = 0;

	// 每秒采样（按 UTC 整秒对齐，便于跨进程配对）
	int64 LastSampleSecond = 0;

	// Client 观测
	bool bClientInWorldEmitted = false;
	TMap<TWeakObjectPtr<ASwarmBugPawn>, FBugTrack> BugTracks;
	int32 WindowSlides = 0;
	int32 TotalSlides = 0;
	int64 LastInTotalBytes = -1;
	double LastBytesSampleSeconds = 0.0;
};
