#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "WeaponSnapshot.h"
#include "WeaponMatchSaveGame.generated.h"

inline constexpr int32 WeaponMatchSaveGameVersion = 1;

/**
 * 可恢复战局的数据包。
 * SaveGame 头保存加载 World/创建 Session 前必须知道的固定配置，
 * Snapshot 只保存目标 World 内需要 Apply 的可变权威状态。
 */
UCLASS()
class LETSDEEPROCK_API UWeaponMatchSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	UPROPERTY(SaveGame)
	int32 SaveFormatVersion = WeaponMatchSaveGameVersion;

	UPROPERTY(SaveGame)
	FString MapPackageName;

	UPROPERTY(SaveGame)
	int32 MaxPlayers = 4;

	UPROPERTY(SaveGame)
	FWeaponSnapshot Snapshot;
};
