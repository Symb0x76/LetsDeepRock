// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/HUD.h"
#include "WeaponDebugTypes.h"
#include "WeaponHUD.generated.h"

/**
 *  俯视角武器系统调试 HUD（Variant_Weapon）
 *
 *  职责：
 *  - 左上角绘制当前装备武器的名称 / 配置 ID，供 PIE 中直观查看切枪效果
 *  - 每帧从本地玩家 Pawn 上的 UWeaponEquipmentComponent 实时读取当前武器数据，
 *    切枪后自动跟随更新，无需额外事件绑定或缓存
 *
 *  仅做最小可用的调试绘制，不引入 UMG / 通用 HUD 框架。
 */
UCLASS()
class AWeaponHUD : public AHUD
{
	GENERATED_BODY()

public:
	/** 接收本地 PlayerController 转发的权威伤害事件。 */
	void AddWeaponDamageDebugEvent(const FWeaponDamageDebugEvent& Event);

protected:

	//~ AHUD
	virtual void DrawHUD() override;

	/** 绘制左上角当前武器实际配置链路。 */
	void DrawCurrentWeaponInfo();
	void DrawNPCDebugInfo();
	void DrawDamageEvents();
	void DrawLastDamageDetails();

private:
	struct FActiveDamageNumber
	{
		FWeaponDamageDebugEvent Event;
		float StartTime = 0.0f;
	};

	TArray<FActiveDamageNumber> ActiveDamageNumbers;
	FWeaponDamageDebugEvent LastDamageEvent;
	bool bHasLastDamageEvent = false;
};
