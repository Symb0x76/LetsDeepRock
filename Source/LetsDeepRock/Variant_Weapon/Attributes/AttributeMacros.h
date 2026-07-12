// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

// 标准 GAS 属性访问器宏：为每个属性生成 Getter/Setter/Initter
// 所有 AttributeSet 子类通过 include 本头文件获取该宏，避免重复定义
#define ATTRIBUTE_ACCESSORS(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_PROPERTY_GETTER(ClassName, PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_GETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_SETTER(PropertyName) \
	GAMEPLAYATTRIBUTE_VALUE_INITTER(PropertyName)
