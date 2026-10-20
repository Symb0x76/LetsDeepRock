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
	/** 绘制右下角弹药测试面板（弹匣/容量、备弹、换弹状态）。 */
	void DrawAmmoPanel();
	/** 绘制右下角蓄力条（仅蓄力射击进行中显示）。这是玩法 UI，不受 Weapon Debug UI 开关影响，
	 *  始终按蓄力状态绘制；bDebugUIEnabled 仅用于决定 Y 位置（Debug 面板开启时让位到弹药面板上方，
	 *  关闭时贴近右下角）。 */
	void DrawChargeFireBar(bool bDebugUIEnabled);

	/** 画面顶部中央的任务/撤离状态行(A1 玩法 UI,不受 Debug 开关影响)。 */
	void DrawObjectiveStatus();

	/**
	 *  B2:敌人头顶血条/护盾条 + 状态图标(玩法 UI)。只给受伤或带状态的敌人画,避免满场血条;
	 *  Debug 开关打开时由 DrawNPCDebugInfo 的详细标签代替。数据全部来自复制:
	 *  NPC AttributeSet 自身复制属性,Minimal 模式下状态 GE 授予的 Tag 仍复制到客户端。
	 */
	void DrawEnemyOverheadBars();

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
