#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "WeaponSnapshotCoordinator.generated.h"

class UAbilitySystemComponent;
class APlayerState;
class UWeaponMatchSaveGame;
struct FUniqueNetIdRepl;
struct FGameplayTag;
struct FWeaponSnapshot;

/** 服务器权威快照入口；负责捕获一致性边界和恢复前校验 */
UCLASS()
class LETSDEEPROCK_API UWeaponSnapshotCoordinator : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	/** 捕获服务器权威状态以及恢复 World/Session 所需的固定配置 */
	bool CaptureSaveGame(const UWorld* World, UWeaponMatchSaveGame*& OutSaveGame, FString& OutError);
	static bool ValidateSaveGame(const UWeaponMatchSaveGame* SaveGame, FString& OutError);
	bool RestoreSaveGame(const UWeaponMatchSaveGame* SaveGame, FString& OutError);

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
	static bool ValidatePlayers(const FWeaponSnapshot& Snapshot, const int32 MaxPlayers, FString& OutError);

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

	static FUniqueNetIdRepl ResolvePlayerId(const APlayerState* PlayerState);
	static FUniqueNetIdRepl ResolvePlayerIdFromActor(const AActor* Actor);

	static bool IsFiniteVector(const FVector& Value);
	static bool IsFiniteRotator(const FRotator& Value);

	/** 当前服务器战局身份；跨地图保留，不能在每次 Capture 时重新生成 */
	FGuid ActiveMatchId;
	int64 NextSnapshotRevision = 1;

	/** 保持最近捕获或反序列化的 SaveGame 存活，直到调用方接管或下一包替换它。 */
	UPROPERTY(Transient)
	TObjectPtr<UWeaponMatchSaveGame> RetainedSaveGame;
};
