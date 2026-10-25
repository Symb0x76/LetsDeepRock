// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Online/WeaponRecoveryRoster.h"
#include "WeaponGameState.h"
#include "WeaponGameMode.generated.h"

class AWeaponCharacter;
class AWeaponPlayerController;
class AGridMapObjectiveActor;
class AGridMapExtractionActor;
class UWeaponWaveDirectorComponent;
class AWeaponPlayerState;
class AWeaponProjectile;
struct FWeaponObjectiveSnapshot;
enum class EWeaponMatchPhase : uint8;

/**
 *  俯视角武器系统 Game Mode（Variant_Weapon）
 *
 *  职责：
 *  - 指定默认 Pawn / Controller
 *  - 玩家死亡 → 重生流程管理（阶段 5）
 */
UCLASS(abstract)
class AWeaponGameMode : public AGameModeBase
{
	GENERATED_BODY()

public:

	AWeaponGameMode();
	virtual void InitGame(
		const FString& MapName,
		const FString& Options,
		FString& ErrorMessage) override;
	virtual void InitGameState() override;

	virtual void PreLogin(const FString& Options, const FString& Address, const FUniqueNetIdRepl& UniqueId, FString& ErrorMessage) override;

	virtual void Logout(AController* ExitingController) override;
	virtual void RestartPlayer(AController* NewPlayer) override;

	/** Import one complete recovery roster after validating every entry and connected identity. */
	bool SetRecoveryRoster(const TArray<FWeaponRecoveryRosterSnapshot>& NewRoster, uint64 RestoredRevision);

	uint64 GetRosterRevision() const { return RosterRevision; }
	const TMap<FUniqueNetIdRepl, FWeaponRecoveryRosterEntry>& GetRecoveryRoster() const { return RecoveryRoster; }
	float GetPendingGracePeriodSeconds() const { return PendingGracePeriodSeconds; }
	bool MarkPlayerDeparted(const FUniqueNetIdRepl& PlayerId);

	/** Close gameplay before Restore; recovery orchestration remains able to tick and use timers. */
	bool EnterGameplayRecoveryBarrier();
	/** Open gameplay only after the caller has verified the recovery attempt invariants. */
	bool ReleaseGameplayRecoveryBarrier();
	/** Apply Snapshot phase/deadline without starting the phase timer while the barrier is closed. */
	void ApplyRecoveredMatchPhase(EWeaponMatchPhase Phase, float RemainingPhaseTime);

	/** Snapshot v10:目标/撤离进度(时间存相对秒数)。 */
	void CaptureObjectiveSnapshot(FWeaponObjectiveSnapshot& OutSnapshot) const;
	/** Snapshot v10 恢复:屏障内重建目标/撤离 Actor 与进度;撤离倒计时以暂停 Timer 交给屏障释放续跑。 */
	void RestoreObjectiveSnapshot(const FWeaponObjectiveSnapshot& Snapshot);
	/** Snapshot v10 恢复:为无 Pawn 的玩家安排复活;屏障关闭时 Timer 暂停,随释放续跑并重新锚定 HUD 时间戳。 */
	void RestorePendingRespawn(AController* PlayerController, float DelaySeconds);
	float GetRespawnDelay() const { return RespawnDelay; }

	/** Intercept PawnLeavingGame when active projectiles still need this Pawn/ASC as source context. */
	bool PreserveLeavingPlayerAttribution(AWeaponPlayerController* LeavingController);
	bool HandleAttributionPlayerStateDeactivated(AWeaponPlayerState* PlayerState);
	void RegisterAttributionProjectile(const FUniqueNetIdRepl& PlayerId, AWeaponProjectile* Projectile);
	void UnregisterAttributionProjectile(const FUniqueNetIdRepl& PlayerId, AWeaponProjectile* Projectile);
	void AdoptRecoveredAttributionTombstone(
		const FUniqueNetIdRepl& PlayerId,
		AWeaponCharacter* Pawn,
		AWeaponPlayerState* PlayerState);
	void GetAttributionTombstoneActors(
		TMap<FUniqueNetIdRepl, AWeaponCharacter*>& OutPawns,
		TMap<FUniqueNetIdRepl, AWeaponPlayerState*>& OutPlayerStates) const;
	
	/**
	 *  玩家死亡通知入口（由 WeaponCharacter::HandleHealthDepleted 调用）
	 *  启动重生倒计时，到期后重生玩家。
	 */
	void PlayerDied(AController* DeadPlayerController);

	/** 所有玩家 ready 且人数达标时，由 Listen Host 发起开局倒计时。 */
	UFUNCTION(BlueprintCallable, Category = "Match")
	bool TryStartWeaponMatch();

	/** Listen Server 大厅开局入口；服务端复核全员准备后携带客户端切图。 */
	UFUNCTION(BlueprintCallable, Category = "Match")
	bool TryStartLobbyTravel();

	/** 由服务端胜负/任务逻辑调用，进入结算阶段。 */
	UFUNCTION(BlueprintCallable, Category = "Match")
	void EndWeaponMatch();


	/** A1:目标点充能完成的服务器入口(由 AGridMapObjectiveActor 调用)。 */
	void HandleObjectiveCompleted();

	/** A2:Combat 房间波次导演(服务器) */
	UPROPERTY(VisibleAnywhere, Category = "Match|Waves")
	TObjectPtr<UWeaponWaveDirectorComponent> WaveDirector;
	/** PlayerState ready 改变后的服务器通知入口。 */
	void HandlePlayerReadyChanged();

	UFUNCTION(BlueprintPure, Category = "Match")
	bool CanStartWeaponMatch() const;

	/**
	 *  立即重生入口（供职业选择 UI 调用）
	 *
	 *  与 PlayerDied 走的延迟重生流程不同：这里跳过 RespawnDelay 倒计时，
	 *  取消任何挂起的死亡重生计时器后立即执行 RespawnPlayer，
	 *  用于"选择职业后立即重生并应用新配置"的场景。
	 *  职业本身的存档写入由调用方（WeaponPlayerController）在此之前完成，
	 *  RespawnPlayer 内部会重新读取存档并应用到新 Pawn。
	 */
	void RespawnPlayerImmediately(AController* PlayerController);

protected:
	/** Last pre-spawn hook: reserve the stable slot before HandleStartingNewPlayer runs. */
	virtual void OnPostLogin(AController* NewPlayer) override;
	virtual APawn* SpawnDefaultPawnAtTransform_Implementation(
		AController* NewPlayer,
		const FTransform& SpawnTransform) override;

	/** 重生延迟（秒），可在蓝图子类中配置 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Respawn")
	float RespawnDelay = 3.0f;

	/** 最少多少名玩家 ready 才能开始；允许 1~4 人合作测试。 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Match", meta = (ClampMin = 1, ClampMax = 4))
	int32 MinPlayersToStart = 1;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Match", meta = (ClampMin = 0.0))
	float StartingCountdownDuration = 3.0f;

	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Match", meta = (ClampMin = 0.0))
	float EndingDuration = 5.0f;

	/** Time a disconnected stable identity retains its original Slot. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Recovery", meta = (ClampMin = 0.0))
	float PendingGracePeriodSeconds = 60.0f;
	
	/**
	* 每个 Controller 独立拥有一个重生计时器。
	* Logout 时必须 ClearTimer 并 Remove，避免持有已断线 Controller。
	*/
	TMap<TObjectPtr<AController>, FTimerHandle> RespawnTimers;
	
	/** 重生玩家：销毁旧 Pawn、生成新 Pawn、恢复状态 */
	void RespawnPlayer(AController* PlayerController);

	/** 设置(或重置)单个玩家的复活 Timer,并写入复制给 HUD 的复活时间戳 */
	void ArmRespawnTimer(AController* PlayerToRespawn, float DelaySeconds);

private:
	bool IsPlayerIdentityConnected(const FUniqueNetIdRepl& PlayerId) const;
	int32 FindSmallestUnreservedPlayerSlot() const;
	/** 无可用 PlayerStart 时按 Slot 计算出生点：以地图 PlayerSpawn 标记（或任一 PlayerStart）为锚，向下找地面。 */
	FVector ComputeFallbackSpawnLocation(AController* NewPlayer, int32 PlayerSlot);
	int32 FindOrReservePlayerSlot(const FUniqueNetIdRepl& PlayerId);
	void MarkPlayerReady(const FUniqueNetIdRepl& PlayerId, int32 OriginalSlot);
	void MarkPlayerPending(const FUniqueNetIdRepl& PlayerId);
	void SchedulePendingGrace(const FUniqueNetIdRepl& PlayerId, float RemainingGraceSeconds);
	void HandlePendingGraceExpired(FUniqueNetIdRepl PlayerId);
	void ClearPendingGraceTimer(const FUniqueNetIdRepl& PlayerId);
	void CleanupCompletedAttributionTombstones(const FUniqueNetIdRepl& PlayerId);
	void ApplyGameplayRecoveryBarrierToWorld(bool bBlocked);
	void ArmMatchPhaseTimer(EWeaponMatchPhase Phase, float RemainingPhaseTime);
	bool RefreshHostMigrationRoles(FString& OutError);

	struct FWeaponAttributionTombstone
	{
		TWeakObjectPtr<AWeaponCharacter> Pawn;
		TWeakObjectPtr<AWeaponPlayerState> PlayerState;
		TSet<TWeakObjectPtr<AWeaponProjectile>> Projectiles;
		bool bPlayerStateDeactivated = false;
	};

	/** Reservations outlive a connection until its roster entry becomes Departed. */
	TMap<FUniqueNetIdRepl, int32> PlayerSlotReservations;
	TMap<FUniqueNetIdRepl, FWeaponRecoveryRosterEntry> RecoveryRoster;
	TMap<FUniqueNetIdRepl, FTimerHandle> PendingGraceTimers;
	TMap<FUniqueNetIdRepl, TSet<TWeakObjectPtr<AWeaponProjectile>>> ActiveAttributionProjectiles;
	TMap<FUniqueNetIdRepl, TArray<FWeaponAttributionTombstone>> AttributionTombstones;
	uint64 RosterRevision = 0;
	float FrozenMatchPhaseRemaining = 0.0f;
	bool bRecoveryWorldRequested = false;
	bool bLobbyTravelRequested = false;

	/** A0：travel options 解析出的任务参数，InitGameState 注入 GameState。 */
	FWeaponMissionSettings ParsedMissionSettings;

	/** 由大厅 ServerTravel 进入（?FromLobby）：大厅已确认全员 ready，任务图内自动 ready 并开局。 */
	bool bArrivedFromLobby = false;
	/** 大厅出发时的人数（?LobbyPlayers）；全员到齐立即开局，否则等待宽限期后开局。 */
	int32 ExpectedLobbyPlayers = 1;
	FTimerHandle LobbyArrivalTimer;
	static constexpr float LobbyArrivalGraceSeconds = 15.0f;
	void TryStartLobbyArrivedMatch();

	/** 玩家加入、退出或 ready 改变时重新验证当前阶段。 */
	void RefreshMatchPhase();

	void BeginWeaponMatch();

	// ---- A1:目标与撤离 ----

	/** 目标完成到撤离开启的倒计时(秒) */
	UPROPERTY(EditDefaultsOnly, Category = "Match|Objective", meta = (ClampMin = "0.0"))
	float ExtractionCountdownSeconds = 20.0f;

	UPROPERTY(EditDefaultsOnly, Category = "Match|Objective")
	TSubclassOf<AGridMapObjectiveActor> ObjectiveActorClass;

	UPROPERTY(EditDefaultsOnly, Category = "Match|Objective")
	TSubclassOf<AGridMapExtractionActor> ExtractionActorClass;

	UPROPERTY()
	TObjectPtr<AGridMapObjectiveActor> ObjectiveActor;

	UPROPERTY()
	TObjectPtr<AGridMapExtractionActor> ExtractionActor;

	FTimerHandle ExtractionTimer;

	/** InGame 开始时在 marker 处生成目标/撤离 actor */
	void SpawnObjectiveActors(bool bForRecovery = false);

	/** 目标倒计时结束:开启撤离区 */
	void OpenExtraction();

	/** 回到 Waiting 前清理目标/撤离 actor 与状态,保证下一局从零开始 */
	void CleanupObjectiveActors();
	void ReturnToWaitingForPlayers();
	void ResetAllPlayerReadyStates();

	FTimerHandle MatchPhaseTimer;
};
