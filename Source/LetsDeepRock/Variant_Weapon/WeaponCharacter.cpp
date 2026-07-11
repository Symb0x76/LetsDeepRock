// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponCharacter.h"
#include "GameFramework/SpringArmComponent.h"
#include "Camera/CameraComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "Components/CapsuleComponent.h"
#include "EnhancedInputComponent.h"
#include "InputAction.h"
#include "InputActionValue.h"
#include "AbilitySystemComponent.h"
#include "WeaponPlayerState.h"
#include "WeaponGameMode.h"
#include "Attributes/WeaponHealthSet.h"
#include "Attributes/AttributeInitHelper.h"
#include "Weapons/WeaponEquipmentComponent.h"
#include "Weapons/WeaponDataAsset.h"
#include "GameplayTagContainer.h"
#include "WeaponGameplayTags.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetMathLibrary.h"
#include "Data/GameDataSubsystem.h"

AWeaponCharacter::AWeaponCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	// 创建相机弹簧臂（俯视角）
	SpringArm = CreateDefaultSubobject<USpringArmComponent>(TEXT("Spring Arm"));
	SpringArm->SetupAttachment(RootComponent);
	SpringArm->SetRelativeRotation(FRotator(-50.0f, 0.0f, 0.0f));
	SpringArm->TargetArmLength = 2200.0f;
	SpringArm->bDoCollisionTest = false;
	SpringArm->bInheritYaw = false;
	SpringArm->bEnableCameraLag = true;
	SpringArm->CameraLagSpeed = 0.5f;

	// 创建相机
	Camera = CreateDefaultSubobject<UCameraComponent>(TEXT("Camera"));
	Camera->SetupAttachment(SpringArm);
	Camera->SetFieldOfView(75.0f);

	// 配置角色移动（俯视角约束到平面）
	GetCharacterMovement()->GravityScale = 1.5f;
	GetCharacterMovement()->MaxAcceleration = 1000.0f;
	GetCharacterMovement()->BrakingFrictionFactor = 1.0f;
	GetCharacterMovement()->bCanWalkOffLedges = false;
	GetCharacterMovement()->bConstrainToPlane = true;
	GetCharacterMovement()->bSnapToPlaneAtStart = true;

	// 3C 优化：面向与移动解耦 —— 不自动转向移动方向，由 Tick 控制朝向鼠标
	GetCharacterMovement()->bOrientRotationToMovement = false;
	GetCharacterMovement()->RotationRate = FRotator(0.0f, 0.0f, 0.0f);
	bUseControllerRotationYaw = false;
}

UAbilitySystemComponent* AWeaponCharacter::GetAbilitySystemComponent() const
{
	// ASC 的真正拥有者是 PlayerState；这里返回缓存指针（可能在初始化前为空）
	return AbilitySystemComponent;
}

void AWeaponCharacter::PossessedBy(AController* NewController)
{
	Super::PossessedBy(NewController);

	// 服务端：Pawn 被 Possess、PlayerState 已就绪，初始化 ASC ActorInfo
	InitAbilitySystem();

	// 服务端：ASC 就绪后，将默认武器列表加入库存并装备第 0 个
	if (UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>())
	{
		for (UWeaponDataAsset* WeaponData : EquipComp->GetDefaultWeapons())
		{
			EquipComp->AddWeaponToInventory(WeaponData);
		}
	}
}

void AWeaponCharacter::OnRep_PlayerState()
{
	Super::OnRep_PlayerState();

	// 客户端：PlayerState 复制到达后初始化 ASC ActorInfo，
	// 用于本地预测、输入绑定与 UI 属性监听。
	InitAbilitySystem();

	// 客户端：ASC 就绪后也设置装备数据（仅设 CurrentWeaponData，不 GiveAbility）
	// 将默认武器列表加入库存，客户端本地装备第 0 个
	if (UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>())
	{
		if (EquipComp->GetWeaponInventory().Num() == 0)
		{
			for (UWeaponDataAsset* WeaponData : EquipComp->GetDefaultWeapons())
			{
				EquipComp->AddWeaponToInventory(WeaponData);
			}
		}
	}
}

void AWeaponCharacter::InitAbilitySystem()
{
	AWeaponPlayerState* PS = GetPlayerState<AWeaponPlayerState>();
	if (!PS)
	{
		return;
	}

	AbilitySystemComponent = PS->GetAbilitySystemComponent();
	if (AbilitySystemComponent)
	{
		// OwnerActor = PlayerState，AvatarActor = 当前 Character
		AbilitySystemComponent->InitAbilityActorInfo(PS, this);
	}

	// 仅服务端、且仅首次初始化属性（避免重生时重复覆盖）
	if (HasAuthority() && !bAttributesInitialized)
	{
		// 从 GameDataSubsystem 获取角色属性配置
		if (UGameInstance* GI = GetGameInstance())
		{
			if (UGameDataSubsystem* DataSys = GI->GetSubsystem<UGameDataSubsystem>())
			{
				const FCharacterAttributeRow* Row = DataSys->GetCharacterAttributes(CharacterConfigRowName);
				if (Row)
				{
					FAttributeInitHelper::InitializePlayerAttributes(AbilitySystemComponent, *Row);
					bAttributesInitialized = true;
				}
			}
		}
	}

	// 仅服务端订阅死亡事件（AddUniqueDynamic 语义：这里用普通委托，先移除再添加避免重复绑定）
	if (HasAuthority())
	{
		if (UWeaponHealthSet* HealthSet = PS->GetHealthSet())
		{
			HealthSet->OnHealthDepleted.RemoveAll(this);
			HealthSet->OnHealthDepleted.AddUObject(this, &AWeaponCharacter::HandleHealthDepleted);
		}
	}
}

void AWeaponCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);

	// 每帧让角色面向鼠标位置
	RotateToMouseCursor(DeltaTime);
}

void AWeaponCharacter::RotateToMouseCursor(float DeltaTime)
{
	// 死亡时不旋转
	if (IsDead())
	{
		return;
	}

	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC)
	{
		return;
	}

	// 将鼠标屏幕坐标射线投射到地面平面（Z = 角色脚底高度）
	FVector WorldLocation, WorldDirection;
	if (!PC->DeprojectMousePositionToWorld(WorldLocation, WorldDirection))
	{
		return;
	}

	// 与角色所在水平面求交（使用角色 Z 坐标作为平面高度）
	const float CharacterZ = GetActorLocation().Z;
	// 避免射线平行于平面（Direction.Z 接近 0）
	if (FMath::Abs(WorldDirection.Z) < KINDA_SMALL_NUMBER)
	{
		return;
	}

	const float T = (CharacterZ - WorldLocation.Z) / WorldDirection.Z;
	// T < 0 表示平面在摄像机后方，忽略
	if (T < 0.0f)
	{
		return;
	}

	const FVector GroundHitPoint = WorldLocation + WorldDirection * T;

	// 计算从角色到鼠标地面点的方向（只关心 XY 平面）
	const FVector ToMouse = GroundHitPoint - GetActorLocation();
	const FVector2D ToMouse2D(ToMouse.X, ToMouse.Y);

	// 鼠标太靠近角色时不旋转（避免抖动）
	if (ToMouse2D.SizeSquared() < 100.0f) // 10cm 死区
	{
		return;
	}

	// 计算目标朝向并平滑插值旋转
	const FRotator TargetRotation = UKismetMathLibrary::MakeRotFromX(FVector(ToMouse2D.X, ToMouse2D.Y, 0.0f));
	const FRotator CurrentRotation = GetActorRotation();
	const FRotator NewRotation = FMath::RInterpTo(CurrentRotation, TargetRotation, DeltaTime, 15.0f);

	SetActorRotation(FRotator(0.0f, NewRotation.Yaw, 0.0f));
}

void AWeaponCharacter::HandleHealthDepleted(AActor* OwningActor)
{
	// 阶段 5：玩家死亡处理
	if (!AbilitySystemComponent)
	{
		return;
	}

	// 1. 添加 State.Dead Tag 到 ASC（阻断所有 Ability 激活）
	AbilitySystemComponent->AddLooseGameplayTag(TAG_State_Dead);

	// 2. 禁用角色移动
	GetCharacterMovement()->DisableMovement();
	GetCharacterMovement()->StopMovementImmediately();

	// 3. 禁用碰撞（避免死亡后挡路/继续被命中）
	GetCapsuleComponent()->SetCollisionEnabled(ECollisionEnabled::NoCollision);

	// 4. 隐藏角色网格（简单的死亡表现，后续可替换为布娃娃/死亡动画）
	SetActorHiddenInGame(true);

	// 5. 卸载当前武器（清除 Ability + 销毁武器 Actor）
	if (UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>())
	{
		EquipComp->UnequipWeapon();
	}

	// 6. 通知 GameMode 启动重生流程
	if (AWeaponGameMode* GM = GetWorld()->GetAuthGameMode<AWeaponGameMode>())
	{
		GM->PlayerDied(GetController());
	}
}

bool AWeaponCharacter::IsDead() const
{
	if (!AbilitySystemComponent)
	{
		return false;
	}
	return AbilitySystemComponent->HasMatchingGameplayTag(TAG_State_Dead);
}

void AWeaponCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	if (UEnhancedInputComponent* EnhancedInput = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		if (MoveAction)
		{
			EnhancedInput->BindAction(MoveAction, ETriggerEvent::Triggered, this, &AWeaponCharacter::Move);
		}

		if (FireAction)
		{
			EnhancedInput->BindAction(FireAction, ETriggerEvent::Triggered, this, &AWeaponCharacter::Fire);
		}

		if (MeleeAction)
		{
			EnhancedInput->BindAction(MeleeAction, ETriggerEvent::Started, this, &AWeaponCharacter::Melee);
		}

		if (SwitchWeaponAction)
		{
			EnhancedInput->BindAction(SwitchWeaponAction, ETriggerEvent::Started, this, &AWeaponCharacter::SwitchWeapon);
		}
	}
}

void AWeaponCharacter::Move(const FInputActionValue& Value)
{
	// 死亡时禁止移动
	if (IsDead())
	{
		return;
	}

	const FVector2D MoveInput = Value.Get<FVector2D>();

	if (Controller != nullptr && Camera != nullptr)
	{
		// 基于摄像机的 Yaw 方向计算移动，使 WASD 始终相对屏幕方向
		const FRotator CameraRotation = Camera->GetComponentRotation();
		const FRotator YawRotation(0.f, CameraRotation.Yaw, 0.f);

		const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
		const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		AddMovementInput(ForwardDirection, MoveInput.Y);
		AddMovementInput(RightDirection, MoveInput.X);
	}
}

void AWeaponCharacter::Fire(const FInputActionValue& Value)
{
	// 死亡时禁止射击
	if (IsDead())
	{
		return;
	}

	// 通过 ASC 尝试激活射击 Ability（由 WeaponEquipmentComponent 授予）
	if (!AbilitySystemComponent)
	{
		return;
	}

	UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>();
	if (EquipComp && EquipComp->GetCurrentWeaponData() && EquipComp->GetCurrentWeaponData()->FireAbilityClass)
	{
		AbilitySystemComponent->TryActivateAbilityByClass(EquipComp->GetCurrentWeaponData()->FireAbilityClass);
	}
}

void AWeaponCharacter::Melee(const FInputActionValue& Value)
{
	// 死亡时禁止近战
	if (IsDead())
	{
		return;
	}

	// 通过 ASC 尝试激活近战 Ability（由 WeaponEquipmentComponent 授予）
	if (!AbilitySystemComponent)
	{
		return;
	}

	UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>();
	if (EquipComp && EquipComp->GetCurrentWeaponData() && EquipComp->GetCurrentWeaponData()->MeleeAbilityClass)
	{
		AbilitySystemComponent->TryActivateAbilityByClass(EquipComp->GetCurrentWeaponData()->MeleeAbilityClass);
	}
}

void AWeaponCharacter::SwitchWeapon(const FInputActionValue& Value)
{
	// 死亡时禁止切枪
	if (IsDead())
	{
		return;
	}

	// 滚轮输入：Axis1D > 0 切下一把，< 0 切上一把
	UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>();
	if (!EquipComp)
	{
		return;
	}

	const float ScrollValue = Value.Get<float>();
	if (ScrollValue > 0.0f)
	{
		EquipComp->SwitchToNext();
	}
	else if (ScrollValue < 0.0f)
	{
		EquipComp->SwitchToPrevious();
	}
}
