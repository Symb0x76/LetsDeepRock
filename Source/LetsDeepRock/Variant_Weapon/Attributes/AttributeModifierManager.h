// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "GameplayEffectTypes.h"
#include "GameplayTagContainer.h"
#include "AttributeModifierManager.generated.h"

class UAbilitySystemComponent;
class UGameplayEffect;

/**
 * 单条修改器定义（从表读取或代码指定）
 */
USTRUCT(BlueprintType)
struct FAttributeModifierDef
{
	GENERATED_BODY()

	/** 目标属性的 GameplayTag（如 "Attr.CritChance"） */
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	FGameplayTag AttributeTag;

	/** 运算类型：Additive=加算, Multiplicitive=乘算, Division=除算, Override=覆盖 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	TEnumAsByte<EGameplayModOp::Type> ModOp = EGameplayModOp::Additive;

	/** 修改数值 */
	UPROPERTY(EditAnywhere, BlueprintReadOnly)
	float Magnitude = 0.0f;
};

/**
 * 通用属性修改器管理组件
 *
 * 管理天赋/模组对角色属性的动态修改。
 * 支持运行时添加/移除修改器，内部通过动态创建 Infinite GE 实现。
 *
 * 用法：挂在 Character 或 PlayerState 上，通过 IAbilitySystemInterface 获取 ASC。
 */
UCLASS(ClassGroup = (GAS), meta = (BlueprintSpawnableComponent))
class UAttributeModifierManager : public UActorComponent
{
	GENERATED_BODY()

public:
	UAttributeModifierManager();

	/**
	 * 应用一组修改器（如一个天赋节点的所有效果）
	 * @param SourceId 来源标识（天赋ID/模组ID），用于后续移除
	 * @param Modifiers 修改器定义列表
	 * @return 是否成功应用（至少一个 Handle 有效）
	 */
	UFUNCTION(BlueprintCallable, Category = "Attributes")
	bool ApplyModifiers(FName SourceId, const TArray<FAttributeModifierDef>& Modifiers);

	/**
	 * 移除指定来源的所有修改器
	 * @param SourceId 之前 Apply 时使用的来源标识
	 */
	UFUNCTION(BlueprintCallable, Category = "Attributes")
	void RemoveModifiers(FName SourceId);

	/** 移除所有修改器 */
	UFUNCTION(BlueprintCallable, Category = "Attributes")
	void RemoveAllModifiers();

	/** 检查指定来源是否已激活 */
	UFUNCTION(BlueprintCallable, Category = "Attributes")
	bool HasModifiers(FName SourceId) const;

private:
	/** 获取 Owner 的 ASC */
	UAbilitySystemComponent* GetASC() const;

	/** 从 GameplayTag 查找对应的 FGameplayAttribute */
	static FGameplayAttribute ResolveAttributeFromTag(const FGameplayTag& Tag);

	/** 已应用的 GE Handle 映射：SourceId → Handle 列表 */
	TMap<FName, TArray<FActiveGameplayEffectHandle>> ActiveModifiers;
};
