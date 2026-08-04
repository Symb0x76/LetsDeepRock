// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponDebugTypes.h"
#include "HAL/IConsoleManager.h"

#if UE_BUILD_SHIPPING
static TAutoConsoleVariable<int32> CVarWeaponDebugUI(
	TEXT("Weapon.DebugUI"), 0,
	TEXT("Shipping 构建中武器调试 UI 被强制禁用。"), ECVF_ReadOnly);
#else
static TAutoConsoleVariable<int32> CVarWeaponDebugUI(
	TEXT("Weapon.DebugUI"), 1,
	TEXT("显示武器测试 Debug UI：0=关闭，1=开启。"), ECVF_Default);
#endif

bool IsWeaponDebugUIEnabled()
{
#if UE_BUILD_SHIPPING
	return false;
#else
	return CVarWeaponDebugUI.GetValueOnGameThread() != 0;
#endif
}

void SetWeaponDebugUIEnabled(bool bEnabled)
{
#if !UE_BUILD_SHIPPING
	CVarWeaponDebugUI->Set(bEnabled ? 1 : 0, ECVF_SetByConsole);
#endif
}
