// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "WeaponWaveDirectorComponent.generated.h"

class AEnemyCharacter;
class AGridMapManager;
struct FSpawnerConfigRow;
struct FWeaponWaveDirectorSnapshot;

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

	/** 虫群条目:由 ISwarmBackend 按行名池化生成,不解析 EnemyClassPath(留空) */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category = "Wave")
	bool bSwarm = false;
};

/**
 *  白盒刷怪导演(A2):Combat 房间触发式波次。
 *
 *  玩家进入 Combat 房间 2D 边界 → 按 DT_SpawnerConfig(难度档位映射行)在房间内
 *  EnemySpawn marker 周围滴流刷怪(组参数:组大小/组间隔/个体间隔/NPC 上限/散布半径)。
 *  服务器权威;恢复屏障期间暂停;局末 ResetWaves 销毁残留并复位房间触发状态。
 *
 *  A2b:难度由 GameMode 开局时从 GameState MissionSettings 注入;目标完成后在撤离区附近
 *  按同一行配额刷一波防御怪(就近 EnemySpawn marker,排除过近点)。
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

	/** 防御波刷怪点离撤离中心的最小距离(避免贴脸刷在玩家脚下) */
	UPROPERTY(EditDefaultsOnly, Category = "Wave", meta = (ClampMin = "0.0"))
	float DefenseMinSpawnDistance = 1000.0f;

	/** A2b:难度注入(GameMode 开局时传入 MissionSettings.Difficulty) */
	void SetDifficulty(int32 InDifficulty);

	/** A2b:目标完成后在撤离中心附近触发一次防御波(配额同当前难度行) */
	void StartDefenseWave(const FVector& Center);

	/** 开局:绑定 Manager,解析敌人类,收集 Combat 房与刷怪点,复位状态 */
	void OnMatchStarted(AGridMapManager* Manager);

	/** 局末:销毁在册敌人并复位(房间触发状态清零) */
	void ResetWaves(bool bDestroySpawned);

	/** Host Migration 快照:难度、各战斗房与防御波进度(时间存相对秒数) */
	void CaptureSnapshotState(FWeaponWaveDirectorSnapshot& OutSnapshot) const;

	/** 恢复:按已恢复的 Recipe 重建房间后按下标写回进度,重建防御波,并接管在场敌人(上限/局末清理) */
	void RestoreSnapshotState(AGridMapManager* Manager, const FWeaponWaveDirectorSnapshot& Snapshot);

	virtual void TickComponent(float DeltaTime, ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction) override;

private:
	/** 一个房间的波次状态 */
	struct FRoomWave
	{
		FBox2D Bounds = FBox2D(ForceInit);
		TArray<FVector> SpawnPoints;
		/** 剩余精英配额(走 SpawnOneEnemy,受 NPCCap 限制) */
		int32 RemainingToSpawn = 0;
		/** 剩余虫群配额(按爆发经 ISwarmBackend 生成,只扣实际生成数) */
		int32 RemainingSwarm = 0;
		double NextSpawnTime = 0.0;
		bool bTriggered = false;
	};

	const FSpawnerConfigRow* GetSpawnerRow() const;
	void TriggerRoom(FRoomWave& Room);
	void SpawnOneEnemy(FRoomWave& Room);
	void SpawnSwarmBurst(FRoomWave& Room);
	int32 PickEnemyEntryIndex() const;
	/** 按权重在 bSwarm 条目中选一行;无可选返回 NAME_None */
	FName PickSwarmRow() const;
	FVector PickSpawnLocation(const FRoomWave& Room) const;

	UPROPERTY()
	TObjectPtr<AGridMapManager> CachedManager;

	TArray<FRoomWave> RoomWaves;

	/** 开局收集的全部 EnemySpawn marker(防御波就近选点用) */
	TArray<FVector> EnemySpawnMarkers;

	/** 已刷出且仍存活/未清理的敌人(局末销毁用) */
	TArray<TWeakObjectPtr<AEnemyCharacter>> SpawnedEnemies;

	int32 Difficulty = 2;

	/** RoomWaves 中前 CombatRoomCount 个为战斗房(顺序由 Recipe 决定),之后为防御波 */
	int32 CombatRoomCount = 0;
	bool bHasDefenseWave = false;
	FVector DefenseCenter = FVector::ZeroVector;

	/** 解析后的敌人类(与 CombatWaveEntries 同下标;无效为 null) */
	TArray<TSubclassOf<AEnemyCharacter>> ResolvedEnemyClasses;
};
