// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerController.h"
#include "WeaponPlayerController.generated.h"

class UInputMappingContext;

/**
 *  俯视角武器系统 Player Controller（Variant_Weapon 骨架）
 *
 *  阶段 0：仅负责注册 Enhanced Input 的 IMC。
 *  后续阶段将扩展：武器输入 InputTag 分发、死亡重生等。
 *
 *  3C 优化：启用鼠标光标显示，角色始终面向鼠标位置。
 */
UCLASS(abstract, Config = "Game")
class AWeaponPlayerController : public APlayerController
{
	GENERATED_BODY()

public:

	AWeaponPlayerController();

protected:

	/** 默认输入映射上下文 */
	UPROPERTY(EditAnywhere, Category = "Input|Input Mappings")
	TArray<UInputMappingContext*> DefaultMappingContexts;

protected:

	/** 初始化输入绑定 */
	virtual void SetupInputComponent() override;
};
