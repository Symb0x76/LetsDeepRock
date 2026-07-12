// Copyright Epic Games, Inc. All Rights Reserved.


#include "DamageExecCalculation.h"
#include "AbilitySystemComponent.h"
#include "Attributes/WeaponHealthSet.h"
#include "Attributes/WeaponCombatSet.h"
#include "Attributes/WeaponResistanceSet.h"
#include "WeaponGameplayTags.h"

// ============================================================================
// 属性捕获定义（标准 GAS ExecCalc 结构体模式）
// ============================================================================

struct FDamageStatics
{
	// --- 源方（攻击方）属性 ---
	DECLARE_ATTRIBUTE_CAPTUREDEF(CritChance);
	DECLARE_ATTRIBUTE_CAPTUREDEF(CritDamage);
	DECLARE_ATTRIBUTE_CAPTUREDEF(ArmorPenetration);
	DECLARE_ATTRIBUTE_CAPTUREDEF(DamageBonus);
	DECLARE_ATTRIBUTE_CAPTUREDEF(WeakpointBonus);

	// --- 目标方（被攻击方）属性 ---
	DECLARE_ATTRIBUTE_CAPTUREDEF(Armor);
	DECLARE_ATTRIBUTE_CAPTUREDEF(ResistPhysical);
	DECLARE_ATTRIBUTE_CAPTUREDEF(ResistFire);
	DECLARE_ATTRIBUTE_CAPTUREDEF(ResistCold);
	DECLARE_ATTRIBUTE_CAPTUREDEF(ResistPoison);
	DECLARE_ATTRIBUTE_CAPTUREDEF(ResistExplosion);
	DECLARE_ATTRIBUTE_CAPTUREDEF(ResistElectric);

	FDamageStatics()
	{
		// 源方：从 CombatSet 捕获，不拍快照（实时读取）
		DEFINE_ATTRIBUTE_CAPTUREDEF(UWeaponCombatSet, CritChance, Source, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UWeaponCombatSet, CritDamage, Source, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UWeaponCombatSet, ArmorPenetration, Source, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UWeaponCombatSet, DamageBonus, Source, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UWeaponCombatSet, WeakpointBonus, Source, false);

		// 目标方：从 HealthSet 和 ResistanceSet 捕获
		DEFINE_ATTRIBUTE_CAPTUREDEF(UWeaponHealthSet, Armor, Target, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UWeaponResistanceSet, ResistPhysical, Target, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UWeaponResistanceSet, ResistFire, Target, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UWeaponResistanceSet, ResistCold, Target, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UWeaponResistanceSet, ResistPoison, Target, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UWeaponResistanceSet, ResistExplosion, Target, false);
		DEFINE_ATTRIBUTE_CAPTUREDEF(UWeaponResistanceSet, ResistElectric, Target, false);
	}
};

static const FDamageStatics& DamageStatics()
{
	static FDamageStatics Statics;
	return Statics;
}

// ============================================================================
// 构造：注册所有需要捕获的属性
// ============================================================================

UDamageExecCalculation::UDamageExecCalculation()
{
	RelevantAttributesToCapture.Add(DamageStatics().CritChanceDef);
	RelevantAttributesToCapture.Add(DamageStatics().CritDamageDef);
	RelevantAttributesToCapture.Add(DamageStatics().ArmorPenetrationDef);
	RelevantAttributesToCapture.Add(DamageStatics().DamageBonusDef);
	RelevantAttributesToCapture.Add(DamageStatics().WeakpointBonusDef);
	RelevantAttributesToCapture.Add(DamageStatics().ArmorDef);
	RelevantAttributesToCapture.Add(DamageStatics().ResistPhysicalDef);
	RelevantAttributesToCapture.Add(DamageStatics().ResistFireDef);
	RelevantAttributesToCapture.Add(DamageStatics().ResistColdDef);
	RelevantAttributesToCapture.Add(DamageStatics().ResistPoisonDef);
	RelevantAttributesToCapture.Add(DamageStatics().ResistExplosionDef);
	RelevantAttributesToCapture.Add(DamageStatics().ResistElectricDef);
}

// ============================================================================
// 伤害公式执行
// ============================================================================

void UDamageExecCalculation::Execute_Implementation(
	const FGameplayEffectCustomExecutionParameters& ExecutionParams,
	FGameplayEffectCustomExecutionOutput& OutExecutionOutput) const
{
	const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();
	const FGameplayTagContainer& DamageTags = Spec.GetDynamicAssetTags();

	// ---------- ① 基础伤害（SetByCaller Data.Damage） ----------
	const float BaseDamage = Spec.GetSetByCallerMagnitude(TAG_Data_Damage, false, 0.0f);
	if (BaseDamage <= 0.0f)
	{
		return;
	}

	// ---------- 捕获源方属性 ----------
	const FAggregatorEvaluateParameters EvalParams;

	float CritChance = 0.0f;
	float CritDamage = 1.0f;
	float ArmorPen = 0.0f;
	float DmgBonus = 0.0f;
	float WeakpointBonus = 0.0f;

	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().CritChanceDef, EvalParams, CritChance);
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().CritDamageDef, EvalParams, CritDamage);
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().ArmorPenetrationDef, EvalParams, ArmorPen);
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().DamageBonusDef, EvalParams, DmgBonus);
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().WeakpointBonusDef, EvalParams, WeakpointBonus);

	// ---------- 捕获目标方属性 ----------
	float TargetArmor = 0.0f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().ArmorDef, EvalParams, TargetArmor);

	// ---------- ② 全局伤害加成 ----------
	float Damage = BaseDamage * (1.0f + DmgBonus);

	// ---------- ③ 伤害类型克制修正 ----------
	// TODO: 后续通过 DataTable（FDamageTypeModifierRow）查询类型克制系数，
	// 需要匹配 DamageTags 中的伤害类型 Tag 与目标的护甲类型 Tag
	float TypeModifier = 1.0f;
	Damage *= TypeModifier;

	// ---------- ④ 暴击判定 ----------
	const float CritMult = (FMath::FRand() < FMath::Clamp(CritChance, 0.0f, 1.0f))
		? FMath::Max(CritDamage, 1.0f)
		: 1.0f;
	Damage *= CritMult;

	// ---------- ⑤ 弱点加成 ----------
	// TODO: 检查 GE Spec 是否携带弱点 Tag（如 "Event.Weapon.Hit.Weakpoint"），
	// 若命中弱点则应用 WeakpointBonus 加成
	// 当前暂不实现

	// ---------- ⑥ 护甲减伤 ----------
	const bool bIsTrueDamage = DamageTags.HasTag(TAG_Damage_Type_True);
	if (!bIsTrueDamage)
	{
		// 有效护甲 = 目标护甲 × (1 - 破甲率)，破甲率 clamp [0, 1]
		const float EffectiveArmor = TargetArmor * (1.0f - FMath::Clamp(ArmorPen, 0.0f, 1.0f));
		// DR = Armor / (Armor + K)，K = 200
		const float ArmorDR = EffectiveArmor / (EffectiveArmor + 200.0f);
		Damage *= (1.0f - ArmorDR);
	}

	// ---------- ⑦ 抗性减免 ----------
	if (!bIsTrueDamage)
	{
		float Resistance = 0.0f;

		// 根据 GE Spec 携带的伤害类型 Tag 选择对应抗性
		if (DamageTags.HasTag(TAG_Damage_Type_Fire))
		{
			ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().ResistFireDef, EvalParams, Resistance);
		}
		else if (DamageTags.HasTag(TAG_Damage_Type_Cold))
		{
			ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().ResistColdDef, EvalParams, Resistance);
		}
		else if (DamageTags.HasTag(TAG_Damage_Type_Poison))
		{
			ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().ResistPoisonDef, EvalParams, Resistance);
		}
		else if (DamageTags.HasTag(TAG_Damage_Type_Explosion))
		{
			ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().ResistExplosionDef, EvalParams, Resistance);
		}
		else if (DamageTags.HasTag(TAG_Damage_Type_Electric))
		{
			ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().ResistElectricDef, EvalParams, Resistance);
		}
		else
		{
			// 默认：物理抗性（无 Tag 或 Damage.Type.Physical）
			ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().ResistPhysicalDef, EvalParams, Resistance);
		}

		Damage *= (1.0f - FMath::Clamp(Resistance, 0.0f, 1.0f));
	}

	// ---------- ⑧ 输出到 Damage Meta Attribute ----------
	if (Damage > 0.0f)
	{
		OutExecutionOutput.AddOutputModifier(
			FGameplayModifierEvaluatedData(UWeaponHealthSet::GetDamageAttribute(), EGameplayModOp::Additive, Damage)
		);
	}
}
