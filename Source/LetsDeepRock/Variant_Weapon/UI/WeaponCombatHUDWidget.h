// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "WeaponCombatHUDWidget.generated.h"

class UAbilitySystemComponent;
class UCanvasPanel;
class UProgressBar;
class UTextBlock;
class UVerticalBox;
struct FOnAttributeChangeData;

/**
 *  战斗 HUD(A3,C++ 构建的 UMG,不依赖 Content 资产)。
 *
 *  - 生命/护盾/能量:绑定 PlayerState ASC 的 GetGameplayAttributeValueChangeDelegate(推送)。
 *    ASC 在 PlayerState 上,复活不换 ASC;客户端 PlayerState 晚到时在 NativeTick 中重试绑定。
 *  - 武器/弹药/换弹、手雷冷却、复活倒计时:时间戳或无广播的数据,NativeTick 读取,值变化才 SetText。
 *  - 仅 InGame 阶段显示;整体 HitTestInvisible,不拦截俯视角鼠标瞄准。
 *  - 画布 HUD(AWeaponHUD)的蓄力条/目标状态行保留;调试面板仍由 F8 开关。
 */
UCLASS()
class LETSDEEPROCK_API UWeaponCombatHUDWidget : public UUserWidget
{
	GENERATED_BODY()

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;
	virtual void NativeDestruct() override;

private:
	/** 一条属性条:标签 + 进度条 + 数值 */
	struct FVitalBar
	{
		TObjectPtr<UProgressBar> Bar;
		TObjectPtr<UTextBlock> Value;
	};

	void BuildLayout(UCanvasPanel* RootCanvas);
	FVitalBar AddVitalBar(UVerticalBox* Parent, const FString& Label, const FLinearColor& FillColor);
	UTextBlock* MakeText(const FString& InText, int32 FontSize, const FLinearColor& Color, bool bBold);

	/** 找到本地玩家 PlayerState 的 ASC 并绑定属性变化;已绑定同一 ASC 时直接返回 */
	void TryBindAbilitySystem();
	void UnbindAbilitySystem();
	void HandleVitalAttributeChanged(const FOnAttributeChangeData& ChangeData);
	void RefreshVitals();

	void RefreshWeapon();
	void RefreshGrenadeCooldown();
	void RefreshRespawn();
	void RefreshPhaseVisibility();

	/** 文本变化时才写入,避免每帧重建 Slate 文本 */
	static void SetTextIfChanged(UTextBlock* TextBlock, FString& Cache, const FString& NewText);

	UPROPERTY()
	TObjectPtr<UCanvasPanel> HUDCanvas;

	UPROPERTY()
	TObjectPtr<UTextBlock> WeaponText;

	UPROPERTY()
	TObjectPtr<UTextBlock> AmmoText;

	UPROPERTY()
	TObjectPtr<UTextBlock> GrenadeText;

	UPROPERTY()
	TObjectPtr<UTextBlock> RespawnText;

	FVitalBar HealthBar;
	FVitalBar ShieldBar;
	FVitalBar EnergyBar;

	TWeakObjectPtr<UAbilitySystemComponent> BoundASC;

	FString WeaponTextCache;
	FString AmmoTextCache;
	FString GrenadeTextCache;
	FString RespawnTextCache;
	bool bHUDVisible = true;
};
