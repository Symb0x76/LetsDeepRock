// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponGameplayTags.h"

// 伤害数值（SetByCaller）
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_Damage, "Data.Damage");

// Ability 标识
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Weapon_Fire, "Ability.Weapon.Fire");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Weapon_Melee, "Ability.Weapon.Melee");

// 事件
UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Weapon_Hit, "Event.Weapon.Hit");

// 冷却
UE_DEFINE_GAMEPLAY_TAG(TAG_Cooldown_Weapon_Fire, "Cooldown.Weapon.Fire");
UE_DEFINE_GAMEPLAY_TAG(TAG_Cooldown_Weapon_Melee, "Cooldown.Weapon.Melee");

// 状态
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Dead, "State.Dead");
