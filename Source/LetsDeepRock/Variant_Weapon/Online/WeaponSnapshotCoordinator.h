#pragma once

#include "CoreMinimal.h"
#include "GameFramework/OnlineReplStructs.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "WeaponSnapshotCoordinator.generated.h"

class UAbilitySystemComponent;
class APlayerState;
class AWeaponPlayerController;
class UWeaponMatchSaveGame;
struct FUniqueNetIdRepl;
struct FGameplayTag;
struct FWeaponSnapshot;
struct FWeaponStatusEffectSnapshot;
class AEnemyCharacter;
class AController;
class AWeaponGameMode;
class AWeaponPlayerState;
class AWeaponCharacter;
struct FWeaponPlayerSnapshot;
enum class EWeaponMatchPhase : uint8;

/** 服务器权威快照入口；负责捕获一致性边界和恢复前校验 */
UCLASS()
class LETSDEEPROCK_API UWeaponSnapshotCoordinator : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** Stable logical match identity shared by Snapshot capture and migration role fencing. */
	FGuid GetOrCreateActiveMatchId();
	/** Adopt the Coordinator-issued MatchId before the first local Snapshot is captured. */
	bool AdoptActiveMatchId(const FGuid& MatchId, FString& OutError);
	/** 结束当前逻辑战局(换房间/离开房间时调用),复位 MatchId 与快照修订号,
	 *  否则下一局 AdoptActiveMatchId 会因旧 MatchId 残留而失败。 */
	void ClearActiveMatchId();

	/** 捕获服务器权威状态以及恢复 World/Session 所需的固定配置 */
	bool CaptureSaveGame(const UWorld* World, UWeaponMatchSaveGame*& OutSaveGame, FString& OutError);
	static bool ValidateSaveGame(const UWeaponMatchSaveGame* SaveGame, FString& OutError);
	bool RestoreSaveGame(const UWeaponMatchSaveGame* SaveGame, FString& OutError);
	/** Apply the saved state for a survivor that reconnects after the candidate World was restored. */
	bool RestoreReconnectedPlayer(AWeaponPlayerController* PlayerController, FString& OutError);

	/** v12:把快照里的生存状态(Downed/Dead/Up)应用到服务端玩家;ApplySnapshot 与重连恢复共用。
	 *  Character 为空表示该玩家当前无 Pawn。Phase 为快照的比赛阶段。 */
	static void ApplyPlayerLifeState(
		AWeaponGameMode* GameMode,
		AController* Controller,
		AWeaponPlayerState* PlayerState,
		AWeaponCharacter* Character,
		const FWeaponPlayerSnapshot& Player,
		EWeaponMatchPhase Phase);

	/** SaveGame 序列化/反序列化 */
	static bool SerializeSaveGame(UWeaponMatchSaveGame* SaveGame, TArray<uint8>& OutBytes, FString& OutError);
	bool DeserializeSaveGame(const TArray<uint8>& Bytes, UWeaponMatchSaveGame*& OutSaveGame, FString& OutError);

private:
	struct FRestoreContext;

	bool ValidateRestore(const UWeaponMatchSaveGame* SaveGame, FRestoreContext& OutContext, FString& OutError) const;
	bool StageRestoreActors(const FWeaponSnapshot& Snapshot, FRestoreContext& Context, FString& OutError) const;
	static void CleanupStagedActors(FRestoreContext& Context);
	void ApplySnapshot(const UWeaponMatchSaveGame* SaveGame, FRestoreContext& Context);

	bool CaptureSnapshot(const UWorld* World, FWeaponSnapshot& OutSnapshot, FString& OutError);
	static bool ValidateSnapshot(const FWeaponSnapshot& Snapshot, int32 MaxPlayers, FString& OutError);

	static bool CapturePlayers(const UWorld* World, FWeaponSnapshot& Snapshot, FString& OutError);
	static bool ValidatePlayers(const FWeaponSnapshot& Snapshot, FString& OutError);
	static bool CaptureRecoveryRoster(const UWorld* World, FWeaponSnapshot& Snapshot, FString& OutError);
	static bool ValidateRecoveryRoster(const FWeaponSnapshot& Snapshot, int32 MaxPlayers, FString& OutError);

	static bool CaptureEnemies(const UWorld* World, FWeaponSnapshot& Snapshot, FString& OutError);
	static bool ValidateEnemies(const FWeaponSnapshot& Snapshot, FString& OutError);

	static bool CaptureGridMap(const UWorld* World, FWeaponSnapshot& Snapshot, FString& OutError);
	static bool ValidateGridMap(const FWeaponSnapshot& Snapshot, FString& OutError);

	static bool CaptureProjectiles(const UWorld* World, FWeaponSnapshot& Snapshot, FString& OutError);
	static bool ValidateProjectiles(const FWeaponSnapshot& Snapshot, FString& OutError);

	static TMap<FGameplayTag, float> CaptureAttributeValues(const UAbilitySystemComponent* ASC);
	static bool ValidateAttributeValues(const TMap<FGameplayTag, float>& Values);
	static bool CanRestoreAttributeValues(const UAbilitySystemComponent* ASC, const TMap<FGameplayTag, float>& Values);
	static void RestoreAttributeValues(UAbilitySystemComponent* ASC, const TMap<FGameplayTag, float>& Values);
	/** v10:状态效果为表现与 DoT,恢复失败只告警不中断 Apply */
	static void RestoreEnemyStatusEffects(AEnemyCharacter* Enemy, const TArray<FWeaponStatusEffectSnapshot>& StatusEffects);

	static FUniqueNetIdRepl ResolvePlayerId(const APlayerState* PlayerState);

	static bool IsFiniteVector(const FVector& Value);
	static bool IsFiniteRotator(const FRotator& Value);

	/** 当前服务器战局身份；跨地图保留，不能在每次 Capture 时重新生成 */
	FGuid ActiveMatchId;
	int64 NextSnapshotRevision = 1;

	/** 保持最近捕获或反序列化的 SaveGame 存活，直到调用方接管或下一包替换它。 */
	UPROPERTY(Transient)
	TObjectPtr<UWeaponMatchSaveGame> RetainedSaveGame;

	/** Player identities already applied by the complete candidate-world restore. */
	TSet<FUniqueNetIdRepl> SnapshotAppliedPlayerIds;
	bool bHasActiveRecoverySnapshot = false;
};
