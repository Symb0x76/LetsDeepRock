// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponHealthSet.h"
#include "GameplayEffectExtension.h"
#include "Net/UnrealNetwork.h"
#include "WeaponPlayerController.h"

UWeaponHealthSet::UWeaponHealthSet()
{
	// 默认值（实际初始化应通过初始化 GE 或角色配置覆盖）
	Health = 100.0f;
	MaxHealth = 100.0f;
	Damage = 0.0f;
	Shield = 0.0f;
	MaxShield = 50.0f;
	ShieldRegenRate = 5.0f;
	ShieldRegenDelay = 3.0f;
	Armor = 0.0f;
}

void UWeaponHealthSet::SetPendingWeaponDamageDebug(const FWeaponDamageDebugEvent& Event, AWeaponPlayerController* SourceController)
{
#if !UE_BUILD_SHIPPING
	PendingDamageDebugEvent = Event;
	PendingDamageSourceController = SourceController;
	bHasPendingDamageDebug = SourceController != nullptr;
#endif
}

void UWeaponHealthSet::ConsumePendingWeaponDamageDebug(float ShieldDamage, float HealthDamage)
{
#if !UE_BUILD_SHIPPING
	const bool bShouldSend = bHasPendingDamageDebug;
	FWeaponDamageDebugEvent Event = PendingDamageDebugEvent;
	TWeakObjectPtr<AWeaponPlayerController> SourceController = PendingDamageSourceController;
	bHasPendingDamageDebug = false;
	PendingDamageSourceController.Reset();
	PendingDamageDebugEvent = FWeaponDamageDebugEvent();

	if (bShouldSend)
	{
		Event.ShieldDamage = ShieldDamage;
		Event.HealthDamage = HealthDamage;
		Event.FinalDamage = ShieldDamage + HealthDamage;
		if (AWeaponPlayerController* Controller = SourceController.Get())
		{
			Controller->ClientReceiveWeaponDamageDebugEvent(Event);
		}
	}
#endif
}

void UWeaponHealthSet::ClampHealth(float& NewValue) const
{
	NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxHealth());
}

void UWeaponHealthSet::PreAttributeChange(const FGameplayAttribute& Attribute, float& NewValue)
{
	Super::PreAttributeChange(Attribute, NewValue);

	if (Attribute == GetHealthAttribute())
	{
		ClampHealth(NewValue);
	}
	else if (Attribute == GetMaxHealthAttribute())
	{
		// MaxHealth 至少为 1，避免除零 / clamp 上界为 0
		NewValue = FMath::Max(NewValue, 1.0f);
	}
	else if (Attribute == GetShieldAttribute())
	{
		NewValue = FMath::Clamp(NewValue, 0.0f, GetMaxShield());
	}
	else if (Attribute == GetMaxShieldAttribute())
	{
		NewValue = FMath::Max(NewValue, 0.0f);
	}
	else if (Attribute == GetShieldRegenRateAttribute())
	{
		NewValue = FMath::Max(NewValue, 0.0f);
	}
	else if (Attribute == GetShieldRegenDelayAttribute())
	{
		NewValue = FMath::Max(NewValue, 0.0f);
	}
	else if (Attribute == GetArmorAttribute())
	{
		NewValue = FMath::Max(NewValue, 0.0f);
	}
}

void UWeaponHealthSet::PostGameplayEffectExecute(const FGameplayEffectModCallbackData& Data)
{
	Super::PostGameplayEffectExecute(Data);

	// 处理 Meta Damage 通道：ExecCalc 已计算最终伤害，此处做护盾优先扣减
	if (Data.EvaluatedData.Attribute == GetDamageAttribute())
	{
		const float LocalDamage = GetDamage();
		SetDamage(0.0f);
		float AppliedShieldDamage = 0.0f;
		float AppliedHealthDamage = 0.0f;
		bool bJustDied = false;

		if (LocalDamage > 0.0f)
		{
			float RemainingDamage = LocalDamage;

			// 先扣护盾
			const float OldShield = GetShield();
			if (OldShield > 0.0f && RemainingDamage > 0.0f)
			{
				AppliedShieldDamage = FMath::Min(RemainingDamage, OldShield);
				SetShield(OldShield - AppliedShieldDamage);
				RemainingDamage -= AppliedShieldDamage;
			}

			// 剩余伤害扣生命
			if (RemainingDamage > 0.0f)
			{
				const float OldHealth = GetHealth();
				AppliedHealthDamage = FMath::Min(RemainingDamage, OldHealth);
				const float NewHealth = FMath::Clamp(OldHealth - RemainingDamage, 0.0f, GetMaxHealth());
				SetHealth(NewHealth);

				// 仅在生命值"从 >0 跨越到 <=0"这一刻广播死亡，且必须是服务端权威，
				// 避免重复触发死亡流程（重复销毁 / 重复发奖）。
				const AActor* OwningActor = GetOwningActor();
				const bool bHasAuthority = OwningActor && OwningActor->HasAuthority();
				bJustDied = bHasAuthority && OldHealth > 0.0f && NewHealth <= 0.0f;
			}
		}

		ConsumePendingWeaponDamageDebug(AppliedShieldDamage, AppliedHealthDamage);
		if (bJustDied)
		{
			OnHealthDepleted.Broadcast(GetOwningActor());
		}
	}

	// 处理 MaxHealth 变化：若上限被调低，需回夹当前 Health
	else if (Data.EvaluatedData.Attribute == GetMaxHealthAttribute())
	{
		if (GetHealth() > GetMaxHealth())
		{
			SetHealth(GetMaxHealth());
		}
	}
}

void UWeaponHealthSet::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);

	// Health / MaxHealth 需要复制到客户端；Damage 是 Meta 不复制
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponHealthSet, Health, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponHealthSet, MaxHealth, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponHealthSet, Shield, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponHealthSet, MaxShield, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponHealthSet, ShieldRegenRate, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponHealthSet, ShieldRegenDelay, COND_None, REPNOTIFY_Always);
	DOREPLIFETIME_CONDITION_NOTIFY(UWeaponHealthSet, Armor, COND_None, REPNOTIFY_Always);
}

void UWeaponHealthSet::OnRep_Health(const FGameplayAttributeData& OldHealth)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponHealthSet, Health, OldHealth);
}

void UWeaponHealthSet::OnRep_MaxHealth(const FGameplayAttributeData& OldMaxHealth)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponHealthSet, MaxHealth, OldMaxHealth);
}

void UWeaponHealthSet::OnRep_Shield(const FGameplayAttributeData& OldShield)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponHealthSet, Shield, OldShield);
}

void UWeaponHealthSet::OnRep_MaxShield(const FGameplayAttributeData& OldMaxShield)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponHealthSet, MaxShield, OldMaxShield);
}

void UWeaponHealthSet::OnRep_ShieldRegenRate(const FGameplayAttributeData& OldShieldRegenRate)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponHealthSet, ShieldRegenRate, OldShieldRegenRate);
}

void UWeaponHealthSet::OnRep_ShieldRegenDelay(const FGameplayAttributeData& OldShieldRegenDelay)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponHealthSet, ShieldRegenDelay, OldShieldRegenDelay);
}

void UWeaponHealthSet::OnRep_Armor(const FGameplayAttributeData& OldArmor)
{
	GAMEPLAYATTRIBUTE_REPNOTIFY(UWeaponHealthSet, Armor, OldArmor);
}
