// Copyright Epic Games, Inc. All Rights Reserved.


#include "DamageExecCalculation.h"
#include "AbilitySystemComponent.h"
#include "Attributes/WeaponHealthSet.h"
#include "Attributes/WeaponCombatSet.h"
#include "Attributes/WeaponResistanceSet.h"
#include "WeaponGameplayTags.h"
#include "WeaponGameState.h"
#include "EnemyCharacter.h"
#include "WeaponPlayerController.h"
#include "UI/WeaponDebugTypes.h"
#include "Attributes/StatusEffectComponent.h"
#include "Data/GameDataSubsystem.h"
#include "Engine/GameInstance.h"

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
	if (AWeaponGameState::IsGameplayRecoveryBlockedFor(
		ExecutionParams.GetTargetAbilitySystemComponent()))
	{
		return;
	}

	const FGameplayEffectSpec& Spec = ExecutionParams.GetOwningSpec();
	const FGameplayTagContainer& DamageTags = Spec.GetDynamicAssetTags();
	const bool bBurningDamage = DamageTags.HasTagExact(TAG_Damage_Status_Burning);
	const bool bBleedingDamage = DamageTags.HasTagExact(TAG_Damage_Status_Bleeding);
	FName DamageTypeName = TAG_Damage_Type_Physical.GetTag().GetTagName();
	if (DamageTags.HasTag(TAG_Damage_Type_True))
	{
		DamageTypeName = TAG_Damage_Type_True.GetTag().GetTagName();
	}
	else if (DamageTags.HasTag(TAG_Damage_Type_Fire))
	{
		DamageTypeName = TAG_Damage_Type_Fire.GetTag().GetTagName();
	}
	else if (DamageTags.HasTag(TAG_Damage_Type_Cold))
	{
		DamageTypeName = TAG_Damage_Type_Cold.GetTag().GetTagName();
	}
	else if (DamageTags.HasTag(TAG_Damage_Type_Poison))
	{
		DamageTypeName = TAG_Damage_Type_Poison.GetTag().GetTagName();
	}
	else if (DamageTags.HasTag(TAG_Damage_Type_Explosion))
	{
		DamageTypeName = TAG_Damage_Type_Explosion.GetTag().GetTagName();
	}
	else if (DamageTags.HasTag(TAG_Damage_Type_Electric))
	{
		DamageTypeName = TAG_Damage_Type_Electric.GetTag().GetTagName();
	}

	// ---------- ① 基础伤害（SetByCaller Data.Damage） ----------
	const float BaseDamage = Spec.GetSetByCallerMagnitude(TAG_Data_Damage, false, 0.0f);
	if (BaseDamage <= 0.0f)
	{
		return;
	}
	const float DamageFalloffStart = Spec.GetSetByCallerMagnitude(TAG_Data_DamageFalloffStart, false, 0.0f);
	const float DamageFalloffEnd = Spec.GetSetByCallerMagnitude(TAG_Data_DamageFalloffEnd, false, 0.0f);

	UAbilitySystemComponent* SourceASC = ExecutionParams.GetSourceAbilitySystemComponent();
	UAbilitySystemComponent* TargetASC = ExecutionParams.GetTargetAbilitySystemComponent();
	AActor* SourceAvatar = SourceASC ? SourceASC->GetAvatarActor() : nullptr;
	AActor* TargetAvatar = TargetASC ? TargetASC->GetAvatarActor() : nullptr;
	const FVector SourceLocation = SourceAvatar ? SourceAvatar->GetActorLocation() : FVector::ZeroVector;
	FVector TargetLocation = TargetAvatar ? TargetAvatar->GetActorLocation() : FVector::ZeroVector;
	const FHitResult* HitResult = Spec.GetContext().GetHitResult();
	if (HitResult)
	{
		if (!HitResult->ImpactPoint.IsNearlyZero())
		{
			TargetLocation = HitResult->ImpactPoint;
		}
		else if (!HitResult->Location.IsNearlyZero())
		{
			TargetLocation = HitResult->Location;
		}
	}
	const float HitDistanceCm = SourceAvatar
		? FVector::Distance(SourceLocation, TargetLocation)
		: 0.0f;

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
	const float WeaponArmorPen = Spec.GetSetByCallerMagnitude(TAG_Data_ArmorPenetration, false, 0.0f);
	const float FinalArmorPen = FMath::Clamp(ArmorPen + WeaponArmorPen, 0.0f, 1.0f);
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().DamageBonusDef, EvalParams, DmgBonus);
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().WeakpointBonusDef, EvalParams, WeakpointBonus);

	// ---------- 捕获目标方属性 ----------
	float TargetArmor = 0.0f;
	ExecutionParams.AttemptCalculateCapturedAttributeMagnitude(DamageStatics().ArmorDef, EvalParams, TargetArmor);

	// ---------- ② 全局伤害加成 ----------
	// 状态伤害不继承来源属性；流血还会跳过后续全部外部修正。
	float Damage = (bBurningDamage || bBleedingDamage) ? BaseDamage : BaseDamage * (1.0f + DmgBonus);
	const float DamageBeforeFalloff = Damage;
	float FalloffMultiplier = 1.0f;
	const bool bFalloffEnabled = !bBurningDamage && !bBleedingDamage && SourceAvatar &&
		DamageFalloffEnd > DamageFalloffStart && DamageFalloffEnd > 0.0f;
	if (bFalloffEnabled)
	{
		FalloffMultiplier = 1.0f - FMath::Clamp(
			(HitDistanceCm - DamageFalloffStart) / (DamageFalloffEnd - DamageFalloffStart),
			0.0f,
			1.0f);
		Damage *= FalloffMultiplier;
	}

#if !UE_BUILD_SHIPPING
	const TCHAR* FalloffReason = bFalloffEnabled
		? TEXT("Enabled")
		: (!SourceAvatar
			? TEXT("Disabled:NoSourceAvatar")
			: (DamageFalloffEnd <= 0.0f
				? TEXT("Disabled:FalloffEndNotPositive")
				: TEXT("Disabled:FalloffEndNotGreaterThanStart")));
	UE_LOG(LogTemp, Verbose, TEXT("[FalloffTrace] ExecCalculation BaseDamage=%.3f FalloffStart=%.3f FalloffEnd=%.3f Source=%s Target=%s SourceLocation=%s TargetLocation=%s HasHitResult=%s HitDistanceCm=%.3f FalloffMultiplier=%.6f DamageBeforeFalloff=%.3f DamageAfterFalloff=%.3f Reason=%s"),
		BaseDamage,
		DamageFalloffStart,
		DamageFalloffEnd,
		*GetNameSafe(SourceAvatar),
		*GetNameSafe(TargetAvatar),
		*SourceLocation.ToCompactString(),
		*TargetLocation.ToCompactString(),
		HitResult ? TEXT("true") : TEXT("false"),
		HitDistanceCm,
		FalloffMultiplier,
		DamageBeforeFalloff,
		Damage,
		FalloffReason);
#endif

	// ---------- ③ 伤害类型克制修正 ----------
	float TypeModifier = 1.0f;
	if (!bBleedingDamage && TargetAvatar)
	{
		const UGameInstance* GameInstance = TargetAvatar->GetGameInstance();
		const UGameDataSubsystem* DataSubsystem = GameInstance ? GameInstance->GetSubsystem<UGameDataSubsystem>() : nullptr;
		const FDamageTypeModifierRow* Modifier = DataSubsystem ? DataSubsystem->GetDamageTypeModifier(DamageTypeName) : nullptr;
		if (Modifier && TargetASC)
		{
			if (TargetASC->HasMatchingGameplayTag(TAG_ArmorType_Light))
			{
				TypeModifier = Modifier->VsLight;
			}
			else if (TargetASC->HasMatchingGameplayTag(TAG_ArmorType_Heavy))
			{
				TypeModifier = Modifier->VsHeavy;
			}
			else if (TargetASC->HasMatchingGameplayTag(TAG_ArmorType_Organic))
			{
				TypeModifier = Modifier->VsOrganic;
			}
			else
			{
				TypeModifier = Modifier->VsUnarmored;
			}
		}
	}
	Damage *= TypeModifier;

	// ---------- ④ 暴击判定 ----------
	const bool bCritical = !bBurningDamage && !bBleedingDamage &&
		FMath::FRand() < FMath::Clamp(CritChance, 0.0f, 1.0f);
	const float CritMult = bCritical ? FMath::Max(CritDamage, 1.0f) : 1.0f;
	Damage *= CritMult;

	// ---------- ⑤ 弱点加成 ----------
	// TODO: 检查 GE Spec 是否携带弱点 Tag（如 "Event.Weapon.Hit.Weakpoint"），
	// 若命中弱点则应用 WeakpointBonus 加成
	// 当前暂不实现

	// ---------- ⑥ 护甲减伤 ----------
	const bool bIsTrueDamage = DamageTags.HasTag(TAG_Damage_Type_True);
	float EffectiveArmor = 0.0f;
	float ArmorDR = 0.0f;
	if (!bIsTrueDamage && !bBleedingDamage)
	{
		// 有效护甲 = 目标护甲 × (1 - 破甲率)，破甲率 clamp [0, 1]
		EffectiveArmor = TargetArmor * (1.0f - (bBurningDamage ? 0.0f : FinalArmorPen));
		// DR = Armor / (Armor + K)，K = 200
		ArmorDR = EffectiveArmor / (EffectiveArmor + 200.0f);
		Damage *= (1.0f - ArmorDR);
	}
	const float DamageAfterArmor = Damage;

	if (TargetAvatar && TargetAvatar->ActorHasTag(TEXT("WeaponDamageTestTarget")))
	{
		const AEnemyCharacter* TargetEnemy = Cast<AEnemyCharacter>(TargetAvatar);
		const FName TargetRow = TargetEnemy ? TargetEnemy->GetEnemyConfigRowName() : NAME_None;
		UE_LOG(LogTemp, Warning, TEXT("[ArmorDiag] Hit Target=%s Row=%s BaseDamage=%.3f CapturedArmor=%.3f CharacterArmorPen=%.6f WeaponArmorPen=%.6f FinalArmorPen=%.6f TypeModifier=%.6f EffectiveArmor=%.3f ArmorDR=%.6f DamageAfterArmor=%.3f DamageType=%s Unarmored=%s Light=%s Heavy=%s"),
			*GetNameSafe(TargetAvatar),
			*TargetRow.ToString(),
			BaseDamage,
			TargetArmor,
			ArmorPen,
			WeaponArmorPen,
			FinalArmorPen,
			TypeModifier,
			EffectiveArmor,
			ArmorDR,
			DamageAfterArmor,
			*DamageTypeName.ToString(),
			TargetASC && TargetASC->HasMatchingGameplayTag(TAG_ArmorType_Unarmored) ? TEXT("true") : TEXT("false"),
			TargetASC && TargetASC->HasMatchingGameplayTag(TAG_ArmorType_Light) ? TEXT("true") : TEXT("false"),
			TargetASC && TargetASC->HasMatchingGameplayTag(TAG_ArmorType_Heavy) ? TEXT("true") : TEXT("false"));
	}

	// ---------- ⑦ 抗性减免 ----------
	if (!bIsTrueDamage && !bBleedingDamage)
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

	// 易伤作用于普通直接伤害和燃烧，但明确不作用于流血。
	if (!bBleedingDamage && TargetAvatar)
	{
		if (const UStatusEffectComponent* StatusComponent = TargetAvatar->FindComponentByClass<UStatusEffectComponent>())
		{
			Damage *= StatusComponent->GetVulnerableMultiplier();
		}
	}

	// ---------- ⑧ 输出到 Damage Meta Attribute ----------
	if (Damage > 0.0f)
	{
#if !UE_BUILD_SHIPPING
		AWeaponPlayerController* SourceController = nullptr;
		if (APawn* SourcePawn = Cast<APawn>(SourceAvatar))
		{
			SourceController = Cast<AWeaponPlayerController>(SourcePawn->GetController());
		}

		if (SourceController && TargetASC && IsWeaponDebugUIEnabled())
		{
			if (UWeaponHealthSet* TargetHealthSet = const_cast<UWeaponHealthSet*>(TargetASC->GetSet<UWeaponHealthSet>()))
			{
				FWeaponDamageDebugEvent DebugEvent;
				DebugEvent.DamageType = DamageTypeName;
				DebugEvent.bCritical = bCritical;
				DebugEvent.bWeakpoint = false;
				DebugEvent.TargetName = TargetAvatar ? TargetAvatar->GetName() : TEXT("Unknown");
				if (const AEnemyCharacter* TargetNPC = Cast<AEnemyCharacter>(TargetAvatar))
				{
					if (!TargetNPC->GetEnemyConfigRowName().IsNone())
					{
						DebugEvent.TargetName = TargetNPC->GetEnemyConfigRowName().ToString();
					}
				}

				DebugEvent.TargetLocation = TargetLocation;
				DebugEvent.HitDistanceMeters = HitDistanceCm / 100.0f;
				TargetHealthSet->SetPendingWeaponDamageDebug(DebugEvent, SourceController);
			}
		}
#endif

		OutExecutionOutput.AddOutputModifier(
			FGameplayModifierEvaluatedData(UWeaponHealthSet::GetDamageAttribute(), EGameplayModOp::Additive, Damage)
		);
	}
}
