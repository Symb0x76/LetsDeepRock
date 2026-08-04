// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "WeaponClassSelectWidget.generated.h"

/**
 *  职业选择终端 UI（Variant_Weapon）
 *
 *  设计意图：
 *  - 关卡开始时由 AWeaponPlayerController::BeginPlay 自动创建并 AddToViewport，无需玩家手动呼出。
 *  - 仅提供 Ranger / Demolitionist / Engineer 三个职业卡片。
 *  - 点击卡片 → 调用 SelectProfession(ProfessionId) → 转发到 PlayerController::RequestSelectProfession，
 *    该函数内部会隐藏本 UI 并触发服务端立即重生流程。
 *  - 本类在 NativeOnInitialized 中完全用 C++ UMG 原生构建响应式 UI：
 *    全屏半透明遮罩、SafeZone + ScaleBox 自适应容器、居中三职业横向卡片、
 *    支持 16:9/16:10/21:9 及低分辨率，文字不裁切，不依赖 WBP 蓝图。
 */
UCLASS()
class UWeaponClassSelectWidget : public UUserWidget
{
	GENERATED_BODY()

public:

	/**
	 *  玩家点击某个职业卡片时调用（由 C++ 按钮 OnClicked 绑定）。
	 *  转发到 OwningPlayer 的 PlayerController，由其完成存档写入 + 立即重生 + 隐藏 UI。
	 */
	UFUNCTION(BlueprintCallable, Category = "Profession")
	void SelectProfession(FName ProfessionId);

protected:

	//~ UUserWidget
	virtual void NativeOnInitialized() override;

private:

	// 按钮点击辅助方法（分别对应三个职业）
	UFUNCTION()
	void OnRangerClicked();

	UFUNCTION()
	void OnDemolitionistClicked();

	UFUNCTION()
	void OnEngineerClicked();
};
