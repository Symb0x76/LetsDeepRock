// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "WeaponDebugTypes.generated.h"

/** 服务端权威伤害结算后发送给开火客户端的调试数据。 */
USTRUCT(BlueprintType)
struct FWeaponDamageDebugEvent
{
	GENERATED_BODY()

	UPROPERTY(BlueprintReadOnly)
	float FinalDamage = 0.0f;

	UPROPERTY(BlueprintReadOnly)
	float ShieldDamage = 0.0f;

	UPROPERTY(BlueprintReadOnly)
	float HealthDamage = 0.0f;

	UPROPERTY(BlueprintReadOnly)
	FName DamageType = NAME_None;

	UPROPERTY(BlueprintReadOnly)
	bool bCritical = false;

	UPROPERTY(BlueprintReadOnly)
	bool bWeakpoint = false;

	UPROPERTY(BlueprintReadOnly)
	float HitDistanceMeters = 0.0f;

	UPROPERTY(BlueprintReadOnly)
	FString TargetName;

	UPROPERTY(BlueprintReadOnly)
	FVector_NetQuantize TargetLocation = FVector::ZeroVector;
};

/** 查询/修改 Weapon.DebugUI。Shipping 构建始终返回 false。 */
bool IsWeaponDebugUIEnabled();
void SetWeaponDebugUIEnabled(bool bEnabled);
