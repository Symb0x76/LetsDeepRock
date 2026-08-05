// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponGameplayTags.h"

// 伤害数值（SetByCaller）
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_Damage, "Data.Damage");
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_DamageFalloffStart, "Data.DamageFalloffStart");
UE_DEFINE_GAMEPLAY_TAG(TAG_Data_DamageFalloffEnd, "Data.DamageFalloffEnd");

// Ability 标识
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Weapon, "Ability.Weapon");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Weapon_Fire, "Ability.Weapon.Fire");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Weapon_Melee, "Ability.Weapon.Melee");
UE_DEFINE_GAMEPLAY_TAG(TAG_Ability_Weapon_Grenade, "Ability.Weapon.Grenade");

// 事件
UE_DEFINE_GAMEPLAY_TAG(TAG_Event_Weapon_Hit, "Event.Weapon.Hit");

// 冷却
UE_DEFINE_GAMEPLAY_TAG(TAG_Cooldown_Weapon_Fire, "Cooldown.Weapon.Fire");
UE_DEFINE_GAMEPLAY_TAG(TAG_Cooldown_Weapon_Melee, "Cooldown.Weapon.Melee");
UE_DEFINE_GAMEPLAY_TAG(TAG_Cooldown_Weapon_Grenade, "Cooldown.Weapon.Grenade");

// 状态
UE_DEFINE_GAMEPLAY_TAG(TAG_State_Dead, "State.Dead");

// 伤害类型
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Terrain, "Damage.Terrain");

// --- 属性 Tag ---
// 生存
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_Health, "Attr.Health");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_MaxHealth, "Attr.MaxHealth");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_Shield, "Attr.Shield");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_MaxShield, "Attr.MaxShield");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_ShieldRegenRate, "Attr.ShieldRegenRate");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_ShieldRegenDelay, "Attr.ShieldRegenDelay");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_Armor, "Attr.Armor");

// 战斗
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_CritChance, "Attr.CritChance");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_CritDamage, "Attr.CritDamage");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_ArmorPenetration, "Attr.ArmorPenetration");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_DamageBonus, "Attr.DamageBonus");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_WeakpointBonus, "Attr.WeakpointBonus");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_AttackSpeed, "Attr.AttackSpeed");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_AreaDamageBonus, "Attr.AreaDamageBonus");

// 移动
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_MoveSpeed, "Attr.MoveSpeed");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_SprintSpeed, "Attr.SprintSpeed");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_DodgeDistance, "Attr.DodgeDistance");

// 抗性
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_Resist_Physical, "Attr.Resist.Physical");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_Resist_Fire, "Attr.Resist.Fire");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_Resist_Cold, "Attr.Resist.Cold");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_Resist_Poison, "Attr.Resist.Poison");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_Resist_Explosion, "Attr.Resist.Explosion");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_Resist_Electric, "Attr.Resist.Electric");

// 资源
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_Energy, "Attr.Energy");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_MaxEnergy, "Attr.MaxEnergy");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_EnergyRegenRate, "Attr.EnergyRegenRate");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_Stamina, "Attr.Stamina");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_MaxStamina, "Attr.MaxStamina");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_StaminaRegenRate, "Attr.StaminaRegenRate");
UE_DEFINE_GAMEPLAY_TAG(TAG_Attr_Lucky, "Attr.Lucky");

// 伤害类型
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Type_Physical, "Damage.Type.Physical");
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Type_Fire, "Damage.Type.Fire");
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Type_Cold, "Damage.Type.Cold");
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Type_Poison, "Damage.Type.Poison");
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Type_Explosion, "Damage.Type.Explosion");
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Type_Electric, "Damage.Type.Electric");
UE_DEFINE_GAMEPLAY_TAG(TAG_Damage_Type_True, "Damage.Type.True");

// 敌人护甲类型
UE_DEFINE_GAMEPLAY_TAG(TAG_ArmorType_Unarmored, "ArmorType.Unarmored");
UE_DEFINE_GAMEPLAY_TAG(TAG_ArmorType_Light, "ArmorType.Light");
UE_DEFINE_GAMEPLAY_TAG(TAG_ArmorType_Heavy, "ArmorType.Heavy");
UE_DEFINE_GAMEPLAY_TAG(TAG_ArmorType_Organic, "ArmorType.Organic");
