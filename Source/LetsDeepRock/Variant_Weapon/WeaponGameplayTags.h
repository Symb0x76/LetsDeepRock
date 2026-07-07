// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "NativeGameplayTags.h"

/**
 *  武器系统 Native Gameplay Tags 集中声明
 *
 *  使用 UE_DECLARE_GAMEPLAY_TAG_EXTERN / UE_DEFINE_GAMEPLAY_TAG 代替散落的
 *  FGameplayTag::RequestGameplayTag(FName("...")) 调用。
 *  好处：编译期类型安全、IDE 可跳转、避免拼写错误。
 *
 *  用法：
 *  - .h 中 UE_DECLARE_GAMEPLAY_TAG_EXTERN 声明
 *  - .cpp 中 UE_DEFINE_GAMEPLAY_TAG 定义
 *  - 其他文件 #include "WeaponGameplayTags.h" 后直接使用变量名
 */

// 伤害数值（SetByCaller）
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Data_Damage);

// Ability 标识
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Weapon_Fire);
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Ability_Weapon_Melee);

// 事件
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Event_Weapon_Hit);

// 冷却
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Cooldown_Weapon_Fire);
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Cooldown_Weapon_Melee);

// 状态
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_State_Dead);

// 伤害类型
UE_DECLARE_GAMEPLAY_TAG_EXTERN(TAG_Damage_Terrain);
