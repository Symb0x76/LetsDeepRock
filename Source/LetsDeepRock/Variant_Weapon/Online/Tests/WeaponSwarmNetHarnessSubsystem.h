// Swarm 多进程网络冒烟：命令行驱动的 GameInstance 子系统（不进入 Shipping）

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Online/WeaponSessionSubsystem.h"
#include "Swarm/SwarmSlideTracker.h"
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
 * -WeaponSwarmHarnessFault=FreezeBugs（仅客户端）：进入世界 1 s 后冻结本地虫的平滑，供脚本做阴性对照。
 *
 * UCLASS 声明不能放进 UHT 不认识的 #if 块，故头文件保持无条件；.cpp 实现在 Shipping 下编译为空桩。
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

	UFUNCTION()
	void HandleHostComplete(bool bWasSuccessful, FString Message);

	UFUNCTION()
	void HandleFindComplete(bool bWasSuccessful, const TArray<FWeaponLanSessionResult>& Results);

	UFUNCTION()
	void HandleJoinComplete(bool bWasSuccessful, FString Message);

	bool TickHarness(float DeltaSeconds);
	void DriveSession(UWorld& World);
	void TickHost(UWorld& World);
	void TickClient(UWorld& World, float DeltaSeconds);
	void MakePlayersInvulnerable(UWorld& World) const;
	void RescueFallingPlayers(UWorld& World, const APawn& HostPawn);
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
	/** 回收前要求的最少玩家数（脚本传 3：等晚加入的 Client2 进入后再回收） */
	int32 MinPlayersToRecycle = 0;
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
	double ClientInWorldSeconds = 0.0;
	bool bFaultFreezeBugs = false;
	bool bFaultActiveEmitted = false;
	TMap<TWeakObjectPtr<ASwarmBugPawn>, FSwarmSlideTracker> BugTracks;
	int32 WindowSlides = 0;
	int32 TotalSlides = 0;
	double WindowDtSum = 0.0;
	double WindowDtMax = 0.0;
	int32 WindowFrames = 0;
	int64 LastInTotalBytes = -1;
	double LastBytesSampleSeconds = 0.0;
};
