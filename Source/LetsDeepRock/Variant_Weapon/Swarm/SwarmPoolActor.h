// Swarm 池化虫群：池 Actor（服务器单 Tick 模拟全部虫）

#pragma once

#include "CoreMinimal.h"
#include "AbilitySystemInterface.h"
#include "GameFramework/Actor.h"
#include "Swarm/SwarmSlotPool.h"
#include "Swarm/SwarmSpatialHash.h"
#include "Swarm/SwarmSteering.h"
#include "Swarm/SwarmTypes.h"
#include "Data/DataTableRows.h"
#include "SwarmPoolActor.generated.h"

class AGridMapManager;
class APawn;
class ASwarmBugPawn;
class UAbilitySystemComponent;
class UGameplayEffect;
class UWeaponCombatSet;

/**
 * 虫群池 Actor：仅服务器存在（不复制）。
 * 启动时一次性生成全部 ASwarmBugPawn（失活），死亡即回收槽位，比赛中从不销毁。
 * 自身持有 ASC + 战斗属性集，作为虫攻击玩家时 GE 伤害的来源。
 */
UCLASS(NotPlaceable)
class LETSDEEPROCK_API ASwarmPoolActor : public AActor, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:
	ASwarmPoolActor();

	//~ IAbilitySystemInterface
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;

	/** 一次性生成 Capacity 只失活的虫；重复调用为空操作 */
	void InitPool(int32 Capacity);

	/** 在 Location 周围 150uu 内散布生成，落到 400uu 内的空地；返回实际生成数 */
	int32 SpawnBugs(FName Row, const FVector& Location, int32 Count);

	/** 空间哈希查询（非物理）：对半径内所有虫施加伤害，返回命中数 */
	int32 DamageInRadius(const FVector& Center, float Radius, const FSwarmDamageEvent& Event);

	/** 空间哈希查询（非物理）：对线段上离 Start 最近的一只虫施加伤害，返回 0 或 1 */
	int32 DamageFirstOnLine(const FVector& Start, const FVector& End, const FSwarmDamageEvent& Event);

	int32 NumAlive() const;
	int32 GetCapacity() const { return Pool.Capacity(); }
	void ForEachAliveView(TFunctionRef<void(const FSwarmBugView&)> Fn) const;
	void DespawnAll();

	virtual void Tick(float DeltaSeconds) override;

protected:
	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;

	/** 虫攻击玩家使用的伤害 GE（SetByCaller Data.Damage） */
	UPROPERTY(EditDefaultsOnly, Category = "Swarm")
	TSoftClassPtr<UGameplayEffect> DamageEffectClass;

private:
	/** 每槽位模拟状态；生命值存于 ASwarmBugPawn */
	struct FSwarmSlotState
	{
		float MoveSpeed = 0.f;
		float AttackDamage = 0.f;
		const FSwarmArchetypeTuning* Tuning = nullptr;
		const FEnemyConfigRow* Config = nullptr;
		TWeakObjectPtr<APawn> Target;
		float NextRetargetTime = 0.f;
		float NextAttackTime = 0.f;
		float BurnUntil = 0.f;
		float FrozenUntil = 0.f;
	};

	float GetSimTime() const;
	bool IsTileBlocked(const FVector2D& Point, double Z) const;
	const FEnemyConfigRow& ResolveEnemyConfig(FName Row);

	void HandleBugDamaged(ASwarmBugPawn& Bug, const FSwarmDamageEvent& Event);
	/** 扣血并在 <= 0 时回收；燃烧、爆炸、外部伤害共用 */
	void ApplyHealthLoss(int32 Slot, float Damage);
	void KillSlot(int32 Slot);

	void GatherPlayersAndObstacles();
	void SimulateSlot(int32 Slot, float DeltaSeconds, float SimTime);
	void ApplyDamageEffectToPawn(APawn* TargetPawn, float Damage, const FGameplayTag& DamageTypeTag, const FVector& BugLocation);

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	UPROPERTY()
	TObjectPtr<UWeaponCombatSet> CombatSet;

	UPROPERTY(Transient)
	TSubclassOf<UGameplayEffect> LoadedDamageEffect;

	UPROPERTY(Transient)
	TArray<TObjectPtr<ASwarmBugPawn>> Bugs;

	TWeakObjectPtr<AGridMapManager> GridMapManager;

	FSwarmSlotPool Pool{0};
	FSwarmSpatialHash SpatialHash{200.f};
	TArray<FSwarmSlotState> SlotStates;

	/** 恢复屏障期间累计的暂停时长；SimTime = 世界时间 − PausedTime */
	float PausedTime = 0.f;

	/** DT 行缺失时的回退配置（MaxHealth 30 / MoveSpeed 400 / AttackDamage 10） */
	FEnemyConfigRow FallbackConfig;
	TSet<FName> WarnedMissingRows;

	// 每帧临时缓冲（复用以免分配）
	TArray<int32> ActiveSlots;
	TArray<FVector2D> SlotPositions;
	TArray<TObjectPtr<APawn>> Players;
	TArray<FSwarmObstacle> Obstacles;
	TArray<int32> QueryScratch;
	TArray<FVector2D> NeighborScratch;
};
