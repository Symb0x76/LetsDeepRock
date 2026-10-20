// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "GridMapObjectiveActors.generated.h"

class UStaticMeshComponent;
class AGridMapExtractionActor;

/**
 *  白盒目标点(A1):生成于 Objective marker。
 *
 *  站桩充能玩法(无新增输入绑定):任意玩家站在半径内,充能条按 ChargeSeconds 填满,
 *  填满后服务器通知 GameMode 进入撤离倒计时。恢复屏障期间暂停。
 *  视觉:引擎基础圆柱 + 动态材质染色(未充能黄,完成绿)。
 */
UCLASS()
class LETSDEEPROCK_API AGridMapObjectiveActor : public AActor
{
	GENERATED_BODY()

public:
	AGridMapObjectiveActor();

	virtual void Tick(float DeltaTime) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintPure, Category = "Objective")
	bool IsComplete() const { return bComplete; }

	UFUNCTION(BlueprintPure, Category = "Objective")
	float GetChargeProgress() const { return ChargeProgress; }

	/** Host Migration 恢复:直接写入进度与完成态,不触发 GameMode 的完成流程 */
	void RestoreProgress(float InChargeProgress, bool bInComplete);

protected:
	virtual void BeginPlay() override;

private:
	/** 充能完成 → 服务器通知 GameMode;客户端经复制变绿。 */
	void SetComplete();

	UPROPERTY(VisibleAnywhere, Category = "Objective")
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(ReplicatedUsing = OnRep_Complete)
	bool bComplete = false;

	UFUNCTION()
	void OnRep_Complete();

	/** 充能半径(世界单位) */
	UPROPERTY(EditAnywhere, Category = "Objective", meta = (ClampMin = "50.0"))
	float InteractRadius = 300.0f;

	/** 站满充能所需秒数 */
	UPROPERTY(EditAnywhere, Category = "Objective", meta = (ClampMin = "0.5"))
	float ChargeSeconds = 5.0f;

	float ChargeProgress = 0.0f;
};

/**
 *  白盒撤离区(A1):生成于 PlayerSpawn marker,目标完成前休眠(灰)。
 *  目标完成 + 倒计时结束后激活(绿);所有存活玩家进入半径并滞留 HoldSeconds
 *  即视为撤离成功 → GameMode::EndWeaponMatch()。恢复屏障期间暂停。
 */
UCLASS()
class LETSDEEPROCK_API AGridMapExtractionActor : public AActor
{
	GENERATED_BODY()

public:
	AGridMapExtractionActor();

	virtual void Tick(float DeltaTime) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintPure, Category = "Extraction")
	bool IsActive() const { return bActive; }

	UFUNCTION(BlueprintPure, Category = "Extraction")
	float GetHoldProgress() const { return HoldProgress; }

	/** 目标倒计时结束,服务器激活撤离区 */
	void SetActive();

	/** Host Migration 恢复:撤离区是否已开启与滞留进度 */
	void RestoreState(bool bInActive, float InHoldProgress);

protected:
	virtual void BeginPlay() override;

private:
	UPROPERTY(VisibleAnywhere, Category = "Extraction")
	TObjectPtr<UStaticMeshComponent> Mesh;

	UPROPERTY(ReplicatedUsing = OnRep_Active)
	bool bActive = false;

	UFUNCTION()
	void OnRep_Active();

	/** 撤离判定半径 */
	UPROPERTY(EditAnywhere, Category = "Extraction", meta = (ClampMin = "50.0"))
	float ExtractRadius = 350.0f;

	/** 全员在圈内需滞留的秒数 */
	UPROPERTY(EditAnywhere, Category = "Extraction", meta = (ClampMin = "0.5"))
	float HoldSeconds = 3.0f;

	float HoldProgress = 0.0f;
};
