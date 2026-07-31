// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/GameModeBase.h"
#include "Online/WeaponRecoveryRoster.h"
#include "WeaponGameMode.generated.h"

class AWeaponCharacter;
class AWeaponPlayerController;
class AWeaponPlayerState;
class AWeaponProjectile;
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

	virtual void PostLogin(APlayerController* NewPlayer) override;

	virtual void Logout(AController* ExitingController) override;

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

	/** 由服务端胜负/任务逻辑调用，进入结算阶段。 */
	UFUNCTION(BlueprintCallable, Category = "Match")
	void EndWeaponMatch();

	/** PlayerState ready 改变后的服务器通知入口。 */
	void HandlePlayerReadyChanged();

	UFUNCTION(BlueprintPure, Category = "Match")
	bool CanStartWeaponMatch() const;

protected:

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

private:
	bool IsPlayerIdentityConnected(const FUniqueNetIdRepl& PlayerId) const;
	int32 FindSmallestUnreservedPlayerSlot() const;
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

	/** 玩家加入、退出或 ready 改变时重新验证当前阶段。 */
	void RefreshMatchPhase();

	void BeginWeaponMatch();
	void ReturnToWaitingForPlayers();
	void ResetAllPlayerReadyStates();

	FTimerHandle MatchPhaseTimer;
};
