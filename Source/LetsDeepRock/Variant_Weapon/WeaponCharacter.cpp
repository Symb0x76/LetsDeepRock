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
#include "Weapons/WeaponGrenadeDataAsset.h"
#include "Weapons/GrenadeBallistics.h"
#include "Weapons/GrenadeTrajectoryPreviewComponent.h"
#include "GameplayTagContainer.h"
#include "WeaponGameplayTags.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/KismetMathLibrary.h"
#include "Data/GameDataSubsystem.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "GridMap/GridDamageHelper.h"
#include "GridMap/GridMapManager.h"
#include "Abilities/GameplayAbility.h"
#include "Abilities/GA_ThrowGrenade.h"

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

	// 装备库存与当前武器由服务器复制；客户端不得在这里重建或修改它们。
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
				if (const FCharacterAttributeRow* Row = DataSys->GetCharacterAttributes(CharacterConfigRowName))
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

	// 授予手雷 Ability（独立于武器装备系统，仅服务端授予一次，重生时自动重新授予）
	if (HasAuthority() && GrenadeAbilityClass && !GrenadeAbilityHandle.IsValid())
	{
		FGameplayAbilitySpec GrenadeSpec(GrenadeAbilityClass, 1, INDEX_NONE, this);
		GrenadeAbilityHandle = AbilitySystemComponent->GiveAbility(GrenadeSpec);
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

	const FVector NewAimDirection = FVector(NewRotation.Vector()).GetSafeNormal2D();
	if (HasAuthority())
	{
		SetAimDirectionAuthoritative(NewAimDirection);
	}
	else
	{
		// 本地先更新表现，再把瞄准意图发送给服务器；战斗判定不会读取客户端旋转。
		SetActorRotation(FRotator(0.0f, NewRotation.Yaw, 0.0f));
		ServerSetAimDirection(NewAimDirection);
	}
}

void AWeaponCharacter::SetAimDirectionAuthoritative(const FVector& NewAimDirection)
{
	if (!HasAuthority())
	{
		return;
	}

	const FVector SafeDirection = NewAimDirection.GetSafeNormal2D();
	if (SafeDirection.IsNearlyZero())
	{
		return;
	}

	AimDirection = SafeDirection;
	SetActorRotation(FRotator(0.0f, SafeDirection.Rotation().Yaw, 0.0f));
	ForceNetUpdate();
}

void AWeaponCharacter::ServerSetAimDirection_Implementation(FVector_NetQuantizeNormal NewAimDirection)
{
	// 服务器只接受有限、非零的水平向量，避免客户端提交非法瞄准方向。
	const FVector Candidate = FVector(NewAimDirection);
	if (!FMath::IsFinite(Candidate.X) || !FMath::IsFinite(Candidate.Y) ||
		!FMath::IsFinite(Candidate.Z) || Candidate.SizeSquared2D() < KINDA_SMALL_NUMBER)
	{
		return;
	}

	SetAimDirectionAuthoritative(Candidate);
}

void AWeaponCharacter::OnRep_AimDirection()
{
	const FVector SafeDirection = FVector(AimDirection).GetSafeNormal2D();
	if (!SafeDirection.IsNearlyZero())
	{
		SetActorRotation(FRotator(0.0f, SafeDirection.Rotation().Yaw, 0.0f));
	}
}

void AWeaponCharacter::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(AWeaponCharacter, AimDirection);
}

void AWeaponCharacter::RequestBuildTileAtWorldLocation(const FVector& WorldLocation, EGridTileType BuildType)
{
	if (IsDead())
	{
		return;
	}

	ServerRequestBuildTile(WorldLocation, BuildType);
}

void AWeaponCharacter::ServerRequestBuildTile_Implementation(
	FVector_NetQuantize WorldLocation, EGridTileType BuildType)
{
	if (IsDead() || !FMath::IsFinite(WorldLocation.X) ||
		!FMath::IsFinite(WorldLocation.Y) || !FMath::IsFinite(WorldLocation.Z))
	{
		return;
	}

	if (AGridMapManager* Manager = FGridDamageHelper::FindGridMapManager(this))
	{
		Manager->TryBuildTileAtWorldLocation(this, WorldLocation, BuildType);
	}
}

void AWeaponCharacter::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	// 隐藏预览
	HideGrenadePreview();

	// 仅 Authority 端清除手雷 Ability（重生时新 Character 会重新 Grant）
	if (HasAuthority() && GrenadeAbilityHandle.IsValid() && AbilitySystemComponent)
	{
		AbilitySystemComponent->ClearAbility(GrenadeAbilityHandle);
	}
	GrenadeAbilityHandle = FGameplayAbilitySpecHandle();

	Super::EndPlay(EndPlayReason);
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

	// 4. 清理手雷预览（死亡时不显示预览）
	HideGrenadePreview();

	// 5. 隐藏角色网格（简单的死亡表现，后续可替换为布娃娃/死亡动画）
	SetActorHiddenInGame(true);

	// 6. 卸载当前武器（清除 Ability + 销毁武器 Actor）
	if (UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>())
	{
		EquipComp->UnequipWeapon();
	}

	// 7. 通知 GameMode 启动重生流程
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

		if (ThrowGrenadeAction)
		{
			EnhancedInput->BindAction(ThrowGrenadeAction, ETriggerEvent::Started, this, &AWeaponCharacter::ThrowGrenadeStarted);
			EnhancedInput->BindAction(ThrowGrenadeAction, ETriggerEvent::Triggered, this, &AWeaponCharacter::ThrowGrenadeTriggered);
			EnhancedInput->BindAction(ThrowGrenadeAction, ETriggerEvent::Completed, this, &AWeaponCharacter::ThrowGrenadeCompleted);
			EnhancedInput->BindAction(ThrowGrenadeAction, ETriggerEvent::Canceled, this, &AWeaponCharacter::ThrowGrenadeCanceled);
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

// ============================================================================
// 手雷投掷：预览 + ServerRPC 流程
// ============================================================================

void AWeaponCharacter::ThrowGrenadeStarted(const FInputActionValue& Value)
{
	// 死亡时禁止投掷
	if (IsDead())
	{
		return;
	}

	if (!bEnableGrenadePreview)
	{
		return;
	}

	// 仅本地控制 Pawn 显示预览
	if (!IsLocallyControlled())
	{
		return;
	}

	// 检查冷却（避免预览在冷却期显示）
	if (AbilitySystemComponent && AbilitySystemComponent->HasMatchingGameplayTag(TAG_Cooldown_Weapon_Grenade))
	{
		return;
	}

	if (!GrenadeDataAsset && GrenadeAbilityClass)
	{
		if (const UGA_ThrowGrenade* GrenadeAbilityCDO = Cast<UGA_ThrowGrenade>(GrenadeAbilityClass->GetDefaultObject()))
		{
			GrenadeDataAsset = GrenadeAbilityCDO->GetGrenadeData();
		}
	}

	// 确保预览组件已创建
	if (!GrenadePreviewComponent)
	{
		// 服务器端（Listen Server）也需要创建预览组件用于本地预览
		GrenadePreviewComponent = NewObject<UGrenadeTrajectoryPreviewComponent>(this, TEXT("GrenadePreview"));
		if (GrenadePreviewComponent)
		{
			GrenadePreviewComponent->RegisterComponent();
		}
	}

	bIsPreviewingGrenade = true;
	PreviewGrenadeTarget = FVector::ZeroVector;
	bHasPreviewGrenadeTarget = false;
	ShowGrenadePreview();
}

void AWeaponCharacter::ThrowGrenadeTriggered(const FInputActionValue& Value)
{
	if (!bEnableGrenadePreview || !bIsPreviewingGrenade)
	{
		return;
	}

	UpdateGrenadePreview();
}

void AWeaponCharacter::ThrowGrenadeCompleted(const FInputActionValue& Value)
{
	if (!bEnableGrenadePreview)
	{
		if (!IsDead() && AbilitySystemComponent && GrenadeAbilityClass)
		{
			FVector TargetPoint = FVector::ZeroVector;
			const bool bHasTarget = GetMouseGroundPosition(TargetPoint);

			if (!HasAuthority())
			{
				ServerThrowGrenade(TargetPoint, bHasTarget);
			}
			else
			{
				PendingGrenadeTarget = TargetPoint;
				bHasPendingGrenadeTarget = bHasTarget;
				AbilitySystemComponent->TryActivateAbilityByClass(GrenadeAbilityClass);
			}
		}
		return;
	}

	if (!bIsPreviewingGrenade)
	{
		return;
	}

	bIsPreviewingGrenade = false;

	// 立即隐藏预览
	HideGrenadePreview();

	// 使用最后一次预览解算的目标，确保实际投掷与显示路径一致
	const FVector TargetPoint = PreviewGrenadeTarget;
	const bool bHasTarget = bHasPreviewGrenadeTarget;

	// 客户端→服务器：发送目标点
	if (!HasAuthority())
	{
		ServerThrowGrenade(TargetPoint, bHasTarget);
	}
	else
	{
		// Listen Server / Standalone：直接激活
		if (AbilitySystemComponent && GrenadeAbilityClass)
		{
			PendingGrenadeTarget = TargetPoint;
			bHasPendingGrenadeTarget = bHasTarget;
			AbilitySystemComponent->TryActivateAbilityByClass(GrenadeAbilityClass);
		}
	}
}

void AWeaponCharacter::ThrowGrenadeCanceled(const FInputActionValue& Value)
{
	if (!bEnableGrenadePreview)
	{
		return;
	}

	bIsPreviewingGrenade = false;
	PreviewGrenadeTarget = FVector::ZeroVector;
	bHasPreviewGrenadeTarget = false;

	// 取消：只清理预览，不投掷
	HideGrenadePreview();
}

// === 预览方法 ===

bool AWeaponCharacter::GetMouseGroundPosition(FVector& OutGroundPoint) const
{
	APlayerController* PC = Cast<APlayerController>(GetController());
	if (!PC)
	{
		return false;
	}

	FVector WorldLocation, WorldDirection;
	if (!PC->DeprojectMousePositionToWorld(WorldLocation, WorldDirection))
	{
		return false;
	}

	// 长距离射线检测地面（忽略自身）
	const float TraceDistance = 100000.0f;
	const FVector TraceEnd = WorldLocation + WorldDirection * TraceDistance;

	FCollisionQueryParams QueryParams;
	QueryParams.AddIgnoredActor(this);
	QueryParams.TraceTag = TEXT("GrenadeAim");

	FHitResult TraceHit;
	if (GetWorld()->LineTraceSingleByChannel(TraceHit, WorldLocation, TraceEnd,
		ECC_Visibility, QueryParams) && TraceHit.bBlockingHit)
	{
		OutGroundPoint = TraceHit.ImpactPoint;
		return true;
	}

	// Fallback：Deproject + 角色 Z 平面求交
	const float TargetZ = GetActorLocation().Z;
	if (!FMath::IsNearlyZero(WorldDirection.Z))
	{
		const float T = (TargetZ - WorldLocation.Z) / WorldDirection.Z;
		if (T > 0.0f)
		{
			OutGroundPoint = WorldLocation + WorldDirection * T;
			return true;
		}
	}

	return false;
}

void AWeaponCharacter::ShowGrenadePreview()
{
	if (!GrenadePreviewComponent || !GrenadeDataAsset)
	{
		return;
	}

	// 1. 获取鼠标地面位置
	FVector TargetPoint = FVector::ZeroVector;
	const bool bHasTarget = GetMouseGroundPosition(TargetPoint);
	PreviewGrenadeTarget = TargetPoint;
	bHasPreviewGrenadeTarget = bHasTarget;

	// 2. 获取 2D 瞄准方向
	FVector2D AimDirection2D;
	if (bHasTarget)
	{
		const FVector ToTarget = TargetPoint - GetActorLocation();
		AimDirection2D = FVector2D(ToTarget).GetSafeNormal();
	}
	if (AimDirection2D.SizeSquared() <= KINDA_SMALL_NUMBER)
	{
		const FVector Forward = GetActorForwardVector();
		AimDirection2D = FVector2D(Forward).GetSafeNormal();
	}

	// 3. 计算 Spawn 位置
	const FVector ForwardDir(AimDirection2D.X, AimDirection2D.Y, 0.0f);
	const FVector SpawnLocation = GetActorLocation()
		+ ForwardDir * GrenadeDataAsset->SpawnForwardOffset
		+ FVector(0.0f, 0.0f, GrenadeDataAsset->SpawnUpOffset);

	// 4. 构建弹道解算输入
	FGrenadeBallisticsInput BallisticsInput = FGrenadeBallisticsInput::FromDataAsset(
		GrenadeDataAsset, SpawnLocation, AimDirection2D, TargetPoint, bHasTarget, this);

	// 5. 执行解算
	FGrenadeBallisticsOutput Result;
	FGrenadeBallistics::Calculate(BallisticsInput, GetWorld(), Result);

	// 6. 显示预览
	GrenadePreviewComponent->ShowPreview(Result, GrenadeDataAsset);
}

void AWeaponCharacter::UpdateGrenadePreview()
{
	if (!GrenadePreviewComponent || !GrenadeDataAsset)
	{
		return;
	}

	// 重新获取目标并解算（鼠标可能已移动）
	FVector TargetPoint = FVector::ZeroVector;
	const bool bHasTarget = GetMouseGroundPosition(TargetPoint);
	PreviewGrenadeTarget = TargetPoint;
	bHasPreviewGrenadeTarget = bHasTarget;

	FVector2D AimDirection2D;
	if (bHasTarget)
	{
		const FVector ToTarget = TargetPoint - GetActorLocation();
		AimDirection2D = FVector2D(ToTarget).GetSafeNormal();
	}
	if (AimDirection2D.SizeSquared() <= KINDA_SMALL_NUMBER)
	{
		const FVector Forward = GetActorForwardVector();
		AimDirection2D = FVector2D(Forward).GetSafeNormal();
	}

	const FVector ForwardDir(AimDirection2D.X, AimDirection2D.Y, 0.0f);
	const FVector SpawnLocation = GetActorLocation()
		+ ForwardDir * GrenadeDataAsset->SpawnForwardOffset
		+ FVector(0.0f, 0.0f, GrenadeDataAsset->SpawnUpOffset);

	FGrenadeBallisticsInput BallisticsInput = FGrenadeBallisticsInput::FromDataAsset(
		GrenadeDataAsset, SpawnLocation, AimDirection2D, TargetPoint, bHasTarget, this);

	FGrenadeBallisticsOutput Result;
	FGrenadeBallistics::Calculate(BallisticsInput, GetWorld(), Result);

	GrenadePreviewComponent->UpdatePreview(Result, GrenadeDataAsset);
}

void AWeaponCharacter::HideGrenadePreview()
{
	if (GrenadePreviewComponent)
	{
		GrenadePreviewComponent->HidePreview();
	}
}

// === Server RPC ===

void AWeaponCharacter::ServerThrowGrenade_Implementation(FVector_NetQuantize TargetPoint, bool bHasValidTarget)
{
	// 死亡时拒绝
	if (IsDead())
	{
		return;
	}

	// 基本校验：拒绝 NaN / 距离过远 / 极大值，但不阻断投掷
	// 无效目标时回退到前方投掷（bHasPendingGrenadeTarget=false → GA 用 forward 方向）
	bool bAcceptedTarget = false;
	if (bHasValidTarget)
	{
		// NaN 检查
		if (TargetPoint.ContainsNaN())
		{
			UE_LOG(LogTemp, Warning, TEXT("[ServerThrowGrenade] 目标点包含 NaN，回退到前方投掷。"));
		}
		// 极大值检查
		else if (TargetPoint.SizeSquared() >= FMath::Square(100000.0f))
		{
			UE_LOG(LogTemp, Warning, TEXT("[ServerThrowGrenade] 目标点过大 (%f)，回退到前方投掷。"),
				TargetPoint.Size());
		}
		// 距离检查
		else
		{
			const float MaxValidDistance = GrenadeDataAsset
				? GrenadeDataAsset->MaxThrowRange + GrenadeDataAsset->SpawnForwardOffset + 500.0f
				: 4000.0f;

			const FVector ToTarget = TargetPoint - GetActorLocation();
			if (ToTarget.SizeSquared() > FMath::Square(MaxValidDistance))
			{
				UE_LOG(LogTemp, Warning, TEXT("[ServerThrowGrenade] 目标距离 %f 超出有效范围 %f，回退到前方投掷。"),
					ToTarget.Size(), MaxValidDistance);
			}
			else
			{
				PendingGrenadeTarget = TargetPoint;
				bAcceptedTarget = true;
			}
		}
	}

	bHasPendingGrenadeTarget = bAcceptedTarget;
	if (!bAcceptedTarget)
	{
		PendingGrenadeTarget = FVector::ZeroVector;
	}

	// 激活手雷 Ability
	if (AbilitySystemComponent && GrenadeAbilityClass)
	{
		AbilitySystemComponent->TryActivateAbilityByClass(GrenadeAbilityClass);
	}
}

// === Pending Target 消费接口 ===

bool AWeaponCharacter::ConsumePendingGrenadeTarget(FVector& OutTarget, bool& bOutValid) const
{
	OutTarget = PendingGrenadeTarget;
	bOutValid = bHasPendingGrenadeTarget;

	// 消费后清空（mutable 允许在 const 方法中修改）
	PendingGrenadeTarget = FVector::ZeroVector;
	bHasPendingGrenadeTarget = false;

	return bOutValid;
}
