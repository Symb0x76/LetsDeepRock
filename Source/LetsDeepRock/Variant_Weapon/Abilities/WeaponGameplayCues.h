// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GameplayEffectTypes.h"
#include "WeaponGameplayCues.generated.h"

class UAbilitySystemComponent;
class UMaterialInstanceDynamic;
class UStaticMeshComponent;

/**
 *  B1 GameplayCue 表现层(C++ 处理,无 Cue 资产)。
 *
 *  服务器:Execute*Cue 在 ASC 上触发 Cue;ASC 的 Cue 多播对 Mixed(玩家)与 Minimal(NPC)都下发。
 *  各端:GameplayCueManager::RouteGameplayCue 把 Cue 交给 Avatar 的 IGameplayCueInterface,
 *  Character / NPC / Drone 转发到 HandleCue,在本地生成白盒 FX(引擎基础球体 + 动态材质)。
 *  FX 只是表现,不复制、无碰撞;专用服务器不生成。
 */
namespace WeaponGameplayCues
{
	/** Avatar 的 HandleGameplayCue 统一入口 */
	void HandleCue(AActor* Self, FGameplayTag CueTag, EGameplayCueEvent::Type EventType, const FGameplayCueParameters& Parameters);

	/** 服务器:受击结算后触发命中 Cue(位置取 HitResult,否则取 Avatar 位置) */
	void ExecuteHitCue(UAbilitySystemComponent* TargetASC, const FGameplayEffectContextHandle& EffectContext,
		float AppliedShieldDamage, float AppliedHealthDamage);

	/** 服务器:爆炸 Cue 挂在投掷者/发射者的 ASC 上触发,参数携带中心与半径 */
	void ExecuteExplosionCue(AActor* InstigatorActor, const FVector& Center, float Radius);
}

/** 本地白盒 Cue FX:一次性缩放爆发或附着循环脉冲。不复制。 */
UCLASS(NotBlueprintable, Transient)
class LETSDEEPROCK_API AWeaponCueFXActor : public AActor
{
	GENERATED_BODY()

public:
	AWeaponCueFXActor();

	virtual void Tick(float DeltaSeconds) override;

	/** 一次性:Lifetime 内从 StartScale 放大到 EndScale 后自毁 */
	static AWeaponCueFXActor* SpawnBurst(UWorld* World, const FVector& Location, const FLinearColor& Color,
		float StartScale, float EndScale, float Lifetime);

	/** 循环:附着到 Parent 头顶持续脉冲,直到 Cue 移除或 Parent 失效 */
	static AWeaponCueFXActor* SpawnLoopAttached(AActor* Parent, FGameplayTag CueTag, const FLinearColor& Color, float Scale);

	FGameplayTag GetCueTag() const { return CueTag; }

private:
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> Mesh;

	FGameplayTag CueTag;
	float Age = 0.0f;
	float Lifetime = 0.25f;
	float StartScale = 0.2f;
	float EndScale = 0.5f;
	bool bLooping = false;

	void ApplyColor(const FLinearColor& Color);
};
