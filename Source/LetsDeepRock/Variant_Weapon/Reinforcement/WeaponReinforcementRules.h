// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "UObject/ObjectMacros.h"
#include "WeaponReinforcementRules.generated.h"

class APlayerState;
class AController;
class UWorld;
class UObject;

/** 玩家生存状态:存活 / 倒地(流血计时中,可被救起) / 死亡 */
UENUM(BlueprintType)
enum class EWeaponPlayerLifeState : uint8
{
	Up = 0,
	Downed = 1,
	Dead = 2
};

/** 增援/倒地规则默认值(GameMode 的 UPROPERTY 以此为默认) */
namespace WeaponReinforcementDefaults
{
	constexpr float BleedOutSeconds = 10.f;
	constexpr float AutoReinforceDelay = 30.f;
	constexpr float ReviveSeconds = 3.f;
	constexpr float ReviveRadius = 200.f;
	constexpr float RevivedHealthFraction = 0.3f;
	/** 难度 1/2/3 对应的增援池 */
	inline const int32 PoolByDifficulty[3] = { 8, 6, 4 };
}

/** 难度(夹到 1..3)对应的增援池;数组不足 3 项时用默认值;结果不为负 */
int32 WeaponReinforcementPoolForDifficulty(int32 Difficulty, const TArray<int32>& PoolByDifficulty);

/** 致命一击时是否进入倒地:没有其他存活队友且要求救援者 → 直接死亡 */
bool WeaponShouldEnterDowned(int32 OtherUpPlayers, bool bDownedRequiresReviver);

/** 倒地期间受到的伤害折算为缩短的流血秒数;MaxHealth<=0 返回 BleedOutSeconds;不为负 */
float WeaponDownedDamageToSeconds(float Damage, float MaxHealth, float BleedOutSeconds);

/** 增援池耗尽且无人存活 → 任务失败 */
bool WeaponShouldFailMission(int32 Pool, int32 UpPlayers, int32 ConnectedPlayers);

/**
 * 自动增援的排队名次:条目为 (RespawnServerTime, PlayerId),仅含等待增援的死者。
 * 计时器按时间先后触发(同时刻按 id),每次触发扣一条命;返回 OwnId 的名次(从 0 起),不在队列中返回 INDEX_NONE。
 * 纯函数,HUD 用它判断"池里是否还有轮到我的命"。
 */
int32 WeaponReinforceQueueRank(TConstArrayView<TPair<float, int32>> PendingTimesAndIds, int32 OwnId);

/** 按住交互的救援进度追踪(纯逻辑,便于测试) */
struct FWeaponReviveTracker
{
	/** 推进一帧;目标变化/为空/未按住则从 0 重来。恰在到达 ReviveSeconds 的那一步返回 true,随后复位 */
	bool Advance(const UObject* Target, bool bHeld, float DeltaTime, float ReviveSeconds);
	float GetProgress(float ReviveSeconds) const;
	void Reset();
	const UObject* GetTarget() const { return TargetPtr.Get(); }

private:
	TWeakObjectPtr<const UObject> TargetPtr;
	float Elapsed = 0.f;
};

/** 非 AWeaponPlayerState 或空指针 → Dead(由任务 3 实现) */
EWeaponPlayerLifeState WeaponGetLifeState(const APlayerState* PlayerState);

/** 统计处于 Up 且持有 Pawn 的玩家数,可排除某个控制器(由任务 3 实现) */
int32 WeaponCountUpPlayers(const UWorld* World, const AController* Exclude = nullptr);
