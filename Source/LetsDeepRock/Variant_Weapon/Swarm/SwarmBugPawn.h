// Swarm 池化虫群：池化虫个体（表现、复制、伤害接收）

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Pawn.h"
#include "Swarm/SwarmDamageable.h"
#include "Swarm/SwarmTypes.h"
#include "SwarmBugPawn.generated.h"

class UCapsuleComponent;
class UMaterialInstanceDynamic;
class UStaticMesh;
class UStaticMeshComponent;

/**
 * 池化虫：无 ASC / CharacterMovement / AIController。
 * 位置由池子在服务器直接写入，客户端以 12Hz 复制 + 插值平滑。
 * 生命值仅存服务器（不复制），伤害数学由池子持有。
 */
UCLASS()
class LETSDEEPROCK_API ASwarmBugPawn : public APawn, public ISwarmDamageable
{
	GENERATED_BODY()

public:
	ASwarmBugPawn();

	//~ ISwarmDamageable
	virtual void ReceiveSwarmDamage(const FSwarmDamageEvent& Event) override;

	// ===== 服务器侧接口（池子调用） =====

	/** 激活：重置状态、递增激活序号、传送到出生点并唤醒休眠 */
	void ActivateBug(int32 InSlot, FName InRow, const FVector& Location, float InMaxHealth);

	/** 失活：隐藏、关碰撞、清状态，并让网络进入休眠 */
	void DeactivateBug();

	int32 GetSlot() const { return Slot; }
	bool IsBugActive() const { return bActive; }
	float GetHealth() const { return Health; }
	float GetMaxHealth() const { return MaxHealth; }
	FName GetArchetypeRow() const { return ArchetypeRow; }
	ESwarmStatusFlags GetStatusFlags() const { return static_cast<ESwarmStatusFlags>(StatusFlags); }

	/** 池子写回伤害结算后的生命值（钳制到 [0, MaxHealth]） */
	void SetHealth(float InHealth);

	void SetStatusFlags(ESwarmStatusFlags InFlags);

	/** 池子绑定：ReceiveSwarmDamage 仅在服务器且激活时转发 */
	TFunction<void(ASwarmBugPawn&, const FSwarmDamageEvent&)> OnDamaged;

	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;
	virtual void BeginPlay() override;
	virtual void Tick(float DeltaSeconds) override;
	virtual void PostNetReceiveLocationAndRotation() override;

protected:
	UFUNCTION()
	void OnRep_Active();

	UFUNCTION()
	void OnRep_ArchetypeRow();

	UFUNCTION()
	void OnRep_StatusFlags();

	UFUNCTION()
	void OnRep_ActivationSerial();

private:
	void ApplyActiveVisuals();
	void ApplyArchetypeVisuals();
	void ApplyStatusVisuals();

	/** 激活序号变化时（槽位被回收）直接吸附到最新复制位置，否则只更新平滑目标 */
	void HandleReplicatedLocation();

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UCapsuleComponent> Capsule;

	UPROPERTY(VisibleAnywhere)
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> TintMaterial;

	/** 按 DT_EnemyConfig 行名选择的模型；未配置或资产缺失的行使用锥体 */
	UPROPERTY(EditDefaultsOnly, Category = "Swarm")
	TMap<FName, TSoftObjectPtr<UStaticMesh>> ArchetypeMeshes;

	UPROPERTY(Transient)
	TMap<FName, TObjectPtr<UStaticMesh>> LoadedArchetypeMeshes;

	UPROPERTY(Transient)
	TObjectPtr<UStaticMesh> ConeMeshAsset;

	UPROPERTY(ReplicatedUsing = OnRep_Active)
	bool bActive = false;

	UPROPERTY(ReplicatedUsing = OnRep_ArchetypeRow)
	FName ArchetypeRow;

	UPROPERTY(ReplicatedUsing = OnRep_StatusFlags)
	uint8 StatusFlags = 0;

	UPROPERTY(ReplicatedUsing = OnRep_ActivationSerial)
	uint8 ActivationSerial = 0;

	// 仅服务器
	int32 Slot = INDEX_NONE;
	float Health = 0.f;
	float MaxHealth = 0.f;

	// 仅客户端
	FVector SmoothTargetLocation = FVector::ZeroVector;
	FRotator SmoothTargetRotation = FRotator::ZeroRotator;
	uint8 LastAppliedActivationSerial = 0;
};
