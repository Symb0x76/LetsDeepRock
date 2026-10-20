// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "WeaponWaveDirectorComponent.generated.h"

class AEnemyCharacter;
class AGridMapManager;
struct FSpawnerConfigRow;

/** 敌人类型条目:配置行(属性)+ BP 类软路径(视觉/AI)+ 刷出权重。 */
USTRUCT(BlueprintType)
struct FWeaponEnemyTypeEntry
{
	GENERATED_BODY()

	/** DT_EnemyConfig 行名(BeginPlay 应用属性) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wave")
	FName EnemyRow = TEXT("BasicGrunt");

	/** 敌人 BP 类软路径(Content 归 P4;加载失败则丢弃该条并告警) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wave")
	FSoftClassPath EnemyClassPath;

	/** 相对刷出权重 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wave", meta = (ClampMin = "0.0"))
	float Weight = 1.0f;
};

/**
 *  白盒刷怪导演(A2):Combat 房间触发式波次。
 *
 *  玩家进入 Combat 房间 2D 边界 → 按 DT_SpawnerConfig(难度档位映射行)在房间内
 *  EnemySpawn marker 周围滴流刷怪(组参数:组大小/组间隔/个体间隔/NPC 上限/散布半径)。
 *  服务器权威;恢复屏障期间暂停;局末 ResetWaves 销毁残留并复位房间触发状态。
 *
 *  A2b(待 #70 合入):难度从 GameState MissionSettings 注入;目标完成防御波。
 */
UCLASS(ClassGroup = (Weapon), meta = (BlueprintSpawnableComponent))
class LETSDEEPROCK_API UWeaponWaveDirectorComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UWeaponWaveDirectorComponent();

	/** 难度档位 1/2/3 → DT_SpawnerConfig 行名 */
	UPROPERTY(EditDefaultsOnly, Category = "Wave")
	FName DifficultySpawnerRows[3];

	/** Combat 房波次的敌人构成 */
	UPROPERTY(EditDefaultsOnly, Category = "Wave")
	TArray<FWeaponEnemyTypeEntry> CombatWaveEntries;

	/** 每个已触发房间缓存最近一次选择的刷怪点数量上限 */
	static constexpr int32 MaxCachedSpawnPoints = 8;

	/** A2b:难度注入(暂由配置/调用方给定;#70 合入后接 MissionSettings.Difficulty) */
	void SetDifficulty(int32 InDifficulty);

	/** 开局:绑定 Manager,解析敌人类,收集 Combat 房与刷怪点,复位状态 */
	void OnMatchStarted(AGridMapManager* Manager);

	/** 局末:销毁在册敌人并复位(房间触发状态清零) */
	void ResetWaves(bool bDestroySpawned);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	/** 一个房间的波次状态 */
	struct FRoomWave
	{
		FBox2D Bounds = FBox2D(ForceInit);
		TArray<FVector> SpawnPoints;
		int32 RemainingToSpawn = 0;
		double NextSpawnTime = 0.0;
		bool bTriggered = false;
	};

	const FSpawnerConfigRow* GetSpawnerRow() const;
	void TriggerRoom(FRoomWave& Room);
	void SpawnOneEnemy(FRoomWave& Room);
	int32 PickEnemyEntryIndex() const;
	FVector PickSpawnLocation(const FRoomWave& Room) const;

	UPROPERTY()
	TObjectPtr<AGridMapManager> CachedManager;

	TArray<FRoomWave> RoomWaves;

	/** 已刷出且仍存活/未清理的敌人(局末销毁用) */
	TArray<TWeakObjectPtr<AEnemyCharacter>> SpawnedEnemies;

	int32 Difficulty = 2;

	/** 解析后的敌人类(与 CombatWaveEntries 同下标;无效为 null) */
	TArray<TSubclassOf<AEnemyCharacter>> ResolvedEnemyClasses;
};
