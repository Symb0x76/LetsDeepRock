// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponCharacter.h"
#include "Abilities/WeaponGameplayCues.h"
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
#include "WeaponGameState.h"
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
#include "Vision/VisionSourceComponent.h"
#include "Drone.h"
#include "Engine/World.h"
#include "InputCoreTypes.h"
#include "UObject/ConstructorHelpers.h"
#include "NavigationInvokerComponent.h"

AWeaponCharacter::AWeaponCharacter()
{
	PrimaryActorTick.bCanEverTick = true;

	static ConstructorHelpers::FClassFinder<ADrone> ProfessionToolDroneClassAsset(TEXT("/Game/Variant_Weapon/BluePrints/BP_Ranger_ConstructSpriteDrone"));
	ProfessionToolDroneClass = ProfessionToolDroneClassAsset.Class;

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

	VisionSource = CreateDefaultSubobject<UVisionSourceComponent>(TEXT("VisionSource"));
	NavigationInvoker = CreateDefaultSubobject<UNavigationInvokerComponent>(TEXT("NavigationInvoker"));
	NavigationInvoker->SetGenerationRadii(6000.0f, 8000.0f);

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

	SetGameplayRecoveryBlocked(AWeaponGameState::IsGameplayRecoveryBlockedFor(this));
}

void AWeaponCharacter::Restart()
{
	Super::Restart();

	// GameMode possesses a freshly spawned Character before Restart() installs the default
	// walking/falling mode. If recovery is already blocked, PossessedBy() may therefore have
	// seen MOVE_None. Re-capture the post-Restart mode and keep the barrier authoritative.
	if (HasAuthority() && bGameplayRecoveryBlocked)
	{
		if (UCharacterMovementComponent* Movement = GetCharacterMovement())
		{
			if (Movement->MovementMode != MOVE_None)
			{
				SavedRecoveryMovementMode = static_cast<uint8>(Movement->MovementMode);
				SavedRecoveryCustomMovementMode = Movement->CustomMovementMode;
				bSavedMovementModeForRecovery = true;
			}
			Movement->DisableMovement();
			Movement->StopMovementImmediately();
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

	VisionSource->TeamId = PS->GetTeamId();

	// 每名玩家只读取自身 PlayerState 上的服务端权威职业装备状态。
	if (HasAuthority() && PS->GetProfessionId().IsNone())
	{
		if (UGameInstance* GI = GetGameInstance())
		{
			if (UGameDataSubsystem* DataSys = GI->GetSubsystem<UGameDataSubsystem>())
			{
				if (const FProfessionConfigRow* RangerRow = DataSys->GetProfessionConfig(FName(TEXT("Ranger"))))
				{
					PS->SetProfessionLoadout(FName(TEXT("Ranger")), RangerRow->PrimaryWeaponId, RangerRow->SecondaryWeaponId);
				}
			}
		}
	}
	FName ProfessionId = PS->GetProfessionId();
	FName PrimaryWeaponId = PS->GetPrimaryWeaponId();
	FName SecondaryWeaponId = PS->GetSecondaryWeaponId();
	if (HasAuthority() && ProfessionId.IsNone())
	{
		ProfessionId = FName(TEXT("Ranger"));
	}

	const FCharacterAttributeRow* CharacterRow = nullptr;
	const FProfessionConfigRow* ProfessionRow = nullptr;
	if (UGameInstance* GI = GetGameInstance())
	{
		if (UGameDataSubsystem* DataSys = GI->GetSubsystem<UGameDataSubsystem>())
		{
			ProfessionRow = DataSys->GetProfessionConfig(ProfessionId);
			if (!ProfessionRow && HasAuthority())
			{
				ProfessionId = FName(TEXT("Ranger"));
				ProfessionRow = DataSys->GetProfessionConfig(ProfessionId);
				PrimaryWeaponId = NAME_None;
				SecondaryWeaponId = NAME_None;
			}
			if (ProfessionRow)
			{
				CharacterRow = DataSys->GetCharacterAttributes(ProfessionRow->CharacterAttributeRow);
				if (!CharacterRow && ProfessionId != FName(TEXT("Ranger")))
				{
					ProfessionId = FName(TEXT("Ranger"));
					ProfessionRow = DataSys->GetProfessionConfig(ProfessionId);
					CharacterRow = ProfessionRow ? DataSys->GetCharacterAttributes(ProfessionRow->CharacterAttributeRow) : nullptr;
					PrimaryWeaponId = NAME_None;
					SecondaryWeaponId = NAME_None;
				}
			}
		}
	}
	ActiveCharacterAttributeRow = ProfessionRow ? ProfessionRow->CharacterAttributeRow : NAME_None;
	if (CharacterRow)
	{
		VisionSource->ConfigureCone(CharacterRow->VisionRange, CharacterRow->VisionHalfAngle);
	}

	// 仅服务端、且仅首次初始化属性（避免重生时重复覆盖）
	if (HasAuthority() && !bAttributesInitialized && CharacterRow)
	{
		FAttributeInitHelper::InitializePlayerAttributes(AbilitySystemComponent, *CharacterRow);
		bAttributesInitialized = true;
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

	InitProfessionWeapons(ProfessionRow, PrimaryWeaponId, SecondaryWeaponId);
}

void AWeaponCharacter::RefreshProfessionLoadout()
{
	InitAbilitySystem();
}

bool AWeaponCharacter::SetCharacterConfigRowNameAuthoritative(const FName NewRowName)
{
	// 属性来源只允许职业配置；旧快照字段仅用于确认恢复目标与当前职业一致。
	return HasAuthority() && !NewRowName.IsNone() && NewRowName == ActiveCharacterAttributeRow;
}

void AWeaponCharacter::InitProfessionWeapons(const FProfessionConfigRow* ProfessionRow, FName PrimaryWeaponId, FName SecondaryWeaponId)
{
	bHasSkillItem = ProfessionRow && !ProfessionRow->SkillItemId.IsNone();
	bHasProfessionTool = ProfessionRow && !ProfessionRow->ClassEquipmentId.IsNone();
	if (!HasAuthority()) return;

	UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>();
	if (!EquipComp) return;

	if (ProfessionRow)
	{
		if (EquipComp->GetWeaponInventory().Num() > 0) return;

		if (PrimaryWeaponId.IsNone()) PrimaryWeaponId = ProfessionRow->PrimaryWeaponId;
		if (SecondaryWeaponId.IsNone()) SecondaryWeaponId = ProfessionRow->SecondaryWeaponId;
		UWeaponDataAsset* PrimaryWeapon = EquipComp->ResolveWeaponData(PrimaryWeaponId);
		UWeaponDataAsset* SecondaryWeapon = EquipComp->ResolveWeaponData(SecondaryWeaponId);
		if (PrimaryWeapon && SecondaryWeapon)
		{
			EquipComp->AddWeaponToInventory(PrimaryWeapon);
			EquipComp->AddWeaponToInventory(SecondaryWeapon);
			return;
		}
	}

	// 职业配置或显式资产解析失败时，保留蓝图默认武器作为最小兜底。
	for (UWeaponDataAsset* WeaponData : EquipComp->GetDefaultWeapons())
	{
		EquipComp->AddWeaponToInventory(WeaponData);
	}
}

void AWeaponCharacter::InitializeAttributionTombstone(AWeaponPlayerState* SourcePlayerState)
{
	check(HasAuthority() && SourcePlayerState);

	SetPlayerState(SourcePlayerState);
	InitAbilitySystem();
	if (AbilitySystemComponent)
	{
		AbilitySystemComponent->CancelAllAbilities();
		AbilitySystemComponent->SetUserAbilityActivationInhibited(true);
	}
	if (UWeaponHealthSet* HealthSet = SourcePlayerState->GetHealthSet())
	{
		HealthSet->OnHealthDepleted.RemoveAll(this);
	}
	if (GrenadeAbilityHandle.IsValid() && AbilitySystemComponent)
	{
		AbilitySystemComponent->ClearAbility(GrenadeAbilityHandle);
	}
	GrenadeAbilityHandle = FGameplayAbilitySpecHandle();
	if (UWeaponEquipmentComponent* Equipment = FindComponentByClass<UWeaponEquipmentComponent>())
	{
		Equipment->UnequipWeapon();
	}

	HideGrenadePreview();
	SetActorHiddenInGame(true);
	SetCanBeDamaged(false);
	SetActorEnableCollision(false);
	SetActorTickEnabled(false);
	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		Movement->DisableMovement();
		Movement->StopMovementImmediately();
		Movement->SetComponentTickEnabled(false);
	}
	if (VisionSource)
	{
		VisionSource->bActive = false;
	}
}

void AWeaponCharacter::SetGameplayRecoveryBlocked(const bool bBlocked)
{
	if (!HasAuthority() || bGameplayRecoveryBlocked == bBlocked)
	{
		return;
	}
	bGameplayRecoveryBlocked = bBlocked;

	const FGameplayTagContainer WeaponAbilityTags(TAG_Ability_Weapon);
	if (AbilitySystemComponent)
	{
		if (bBlocked)
		{
			AbilitySystemComponent->CancelAbilities(&WeaponAbilityTags);
			AbilitySystemComponent->BlockAbilitiesWithTags(WeaponAbilityTags);
		}
		else
		{
			AbilitySystemComponent->UnBlockAbilitiesWithTags(WeaponAbilityTags);
		}
	}

	if (UCharacterMovementComponent* Movement = GetCharacterMovement())
	{
		if (bBlocked)
		{
			// MOVE_None is the pre-Restart state of a newly spawned Character, not a
			// usable mode to restore after migration.
			if (Movement->MovementMode != MOVE_None)
			{
				SavedRecoveryMovementMode = static_cast<uint8>(Movement->MovementMode);
				SavedRecoveryCustomMovementMode = Movement->CustomMovementMode;
				bSavedMovementModeForRecovery = true;
			}
			Movement->DisableMovement();
			Movement->StopMovementImmediately();
		}
		else if (!IsDead())
		{
			if (bSavedMovementModeForRecovery)
			{
				Movement->SetMovementMode(
					static_cast<EMovementMode>(SavedRecoveryMovementMode),
					SavedRecoveryCustomMovementMode);
			}
			else if (Movement->MovementMode == MOVE_None)
			{
				// Covers deferred/restored Characters that never received the normal
				// GameMode Restart() lifecycle before the barrier opened.
				Movement->SetDefaultMovementMode();
			}
		}
	}
	if (bBlocked)
	{
		HideGrenadePreview();
	}
	else
	{
		bSavedMovementModeForRecovery = false;
		ForceNetUpdate();
	}
}

void AWeaponCharacter::Tick(float DeltaTime)
{
	Super::Tick(DeltaTime);
	if (AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		HideGrenadePreview();
		return;
	}

	// G 键快速投掷没有 Enhanced Input 的 Triggered 事件,按住期间在 Tick 里刷新预览。
	if (bQuickGrenadeHeld)
	{
		ThrowGrenadeTriggered(FInputActionValue());
	}

	// Recovery blocking is server-owned, so the client-side Character does not run
	// SetGameplayRecoveryBlocked(). A newly replicated Pawn can consequently remain
	// at MOVE_None until the first unrelated movement update (often a fire RPC).
	// Once the replicated barrier is open, initialize the local movement component
	// immediately so WASD does not depend on another gameplay RPC as a kick-start.
	if (UCharacterMovementComponent* Movement = GetCharacterMovement();
		Movement && Movement->MovementMode == MOVE_None && !IsDead())
	{
		Movement->SetDefaultMovementMode();
	}

	// TeamId 可能在 PlayerState 到达后继续复制变化，保持本地视野来源同步。
	if (const AWeaponPlayerState* PS = GetPlayerState<AWeaponPlayerState>())
	{
		VisionSource->TeamId = PS->GetTeamId();
	}

	// 每帧让角色面向鼠标位置
	RotateToMouseCursor(DeltaTime);

	// 平滑恢复相机后坐力偏移（指数插值，约 0.15~0.35 秒明显回弹、0.5 秒内归零）
	if (CurrentRecoilOffset > 0.0f)
	{
		CurrentRecoilOffset = FMath::FInterpTo(CurrentRecoilOffset, 0.0f, DeltaTime, 12.0f);
		if (CurrentRecoilOffset < 0.5f)
		{
			CurrentRecoilOffset = 0.0f;
		}
		SpringArm->TargetArmLength = BaseTargetArmLength - CurrentRecoilOffset;
	}
}

void AWeaponCharacter::RotateToMouseCursor(float DeltaTime)
{
	if (AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}
	// 死亡时不旋转
	if (IsDead() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
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
	if (AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}
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
	DOREPLIFETIME(AWeaponCharacter, CurrentEquipmentSlot);
}

void AWeaponCharacter::RequestBuildTileAtWorldLocation(const FVector& WorldLocation, EGridTileType BuildType)
{
	if (IsDead() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}

	ServerRequestBuildTile(WorldLocation, BuildType);
}

void AWeaponCharacter::ServerRequestBuildTile_Implementation(
	FVector_NetQuantize WorldLocation, EGridTileType BuildType)
{
	if (IsDead() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this) ||
		!FMath::IsFinite(WorldLocation.X) ||
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
	CancelChargeFire();

	// 隐藏预览
	HideGrenadePreview();

	// 仅 Authority 端清除手雷 Ability（重生时新 Character 会重新 Grant）
	if (HasAuthority() && GrenadeAbilityHandle.IsValid() && AbilitySystemComponent)
	{
		AbilitySystemComponent->ClearAbility(GrenadeAbilityHandle);
	}
	GrenadeAbilityHandle = FGameplayAbilitySpecHandle();

	// 无人机是角色部署的独立工具，仅在角色结束生命周期时清理。
	if (HasAuthority() && IsValid(ActiveDrone))
	{
		ActiveDrone->Destroy();
	}
	ActiveDrone = nullptr;

	Super::EndPlay(EndPlayReason);
}

void AWeaponCharacter::HandleHealthDepleted(AActor* OwningActor)
{
	// 阶段 5：玩家死亡处理
	if (!AbilitySystemComponent)
	{
		return;
	}
	CancelChargeFire();

	const UWeaponHealthSet* HealthSet = GetPlayerState<AWeaponPlayerState>()
		? GetPlayerState<AWeaponPlayerState>()->GetHealthSet()
		: nullptr;
	const UWeaponEquipmentComponent* DiagnosticEquipComp = FindComponentByClass<UWeaponEquipmentComponent>();
	UE_LOG(LogTemp, Error,
		TEXT("[DeathTrace] HandleHealthDepleted: Character=%s CallbackOwner=%s Controller=%s Location=%s Health=%.2f Shield=%.2f DeadTagBefore=%d EquipmentSlot=%u WeaponIndex=%d Weapon=%s"),
		*GetNameSafe(this),
		*GetNameSafe(OwningActor),
		*GetNameSafe(GetController()),
		*GetActorLocation().ToCompactString(),
		HealthSet ? HealthSet->GetHealth() : -1.0f,
		HealthSet ? HealthSet->GetShield() : -1.0f,
		AbilitySystemComponent->HasMatchingGameplayTag(TAG_State_Dead) ? 1 : 0,
		CurrentEquipmentSlot,
		DiagnosticEquipComp ? DiagnosticEquipComp->GetCurrentWeaponIndex() : INDEX_NONE,
		*GetNameSafe(DiagnosticEquipComp ? DiagnosticEquipComp->GetCurrentWeaponData() : nullptr));

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
	if (UWeaponEquipmentComponent* DeathEquipComp = FindComponentByClass<UWeaponEquipmentComponent>())
	{
		DeathEquipComp->UnequipWeapon();
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
			EnhancedInput->BindAction(FireAction, ETriggerEvent::Started, this, &AWeaponCharacter::FireStarted);
			EnhancedInput->BindAction(FireAction, ETriggerEvent::Triggered, this, &AWeaponCharacter::FireTriggered);
			EnhancedInput->BindAction(FireAction, ETriggerEvent::Completed, this, &AWeaponCharacter::FireCompleted);
			EnhancedInput->BindAction(FireAction, ETriggerEvent::Canceled, this, &AWeaponCharacter::FireCanceled);
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

	// 四槽使用固定数字键，不依赖测试蓝图或额外 InputAction 资产。
	PlayerInputComponent->BindKey(EKeys::One, IE_Pressed, this, &AWeaponCharacter::SelectPrimary);
	PlayerInputComponent->BindKey(EKeys::Two, IE_Pressed, this, &AWeaponCharacter::SelectSecondary);
	PlayerInputComponent->BindKey(EKeys::Three, IE_Pressed, this, &AWeaponCharacter::SelectSkill);
	PlayerInputComponent->BindKey(EKeys::Four, IE_Pressed, this, &AWeaponCharacter::SelectTool);
	PlayerInputComponent->BindKey(EKeys::G, IE_Pressed, this, &AWeaponCharacter::QuickThrowGrenadePressed);
	PlayerInputComponent->BindKey(EKeys::G, IE_Released, this, &AWeaponCharacter::QuickThrowGrenadeReleased);
	PlayerInputComponent->BindKey(EKeys::R, IE_Pressed, this, &AWeaponCharacter::ReloadWeapon);
	PlayerInputComponent->BindKey(EKeys::B, IE_Pressed, this, &AWeaponCharacter::CycleFireMode);
	PlayerInputComponent->BindKey(EKeys::LeftMouseButton, IE_Pressed, this, &AWeaponCharacter::RawFirePressed);
	PlayerInputComponent->BindKey(EKeys::LeftMouseButton, IE_Released, this, &AWeaponCharacter::RawFireReleased);
}

void AWeaponCharacter::Move(const FInputActionValue& Value)
{
	// 死亡时禁止移动
	if (IsDead() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
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

void AWeaponCharacter::FireStarted(const FInputActionValue& Value)
{
	bFireTriggeredLoggedThisPress = false;
	#if !UE_BUILD_SHIPPING
	LogFireTraceState(TEXT("Enhanced Started"));
	#endif
	if (IsDead() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this)) return;
	ActiveFireInputSlot = CurrentEquipmentSlot;

	if (ActiveFireInputSlot == 2)
	{
		ThrowGrenadeStarted(Value);
	}
	else if (ActiveFireInputSlot == 3)
	{
		DeployProfessionTool();
	}
	else if (IsCurrentWeaponChargeTrigger())
	{
		UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>();
		bIsChargingFire = EquipComp && EquipComp->GetCurrentWeaponData();
		ChargeFireStartTime = GetWorld() ? GetWorld()->GetTimeSeconds() : 0.0;
		ChargeWeaponData = bIsChargingFire ? EquipComp->GetCurrentWeaponData() : nullptr;
		ChargeFireModeId = bIsChargingFire ? EquipComp->GetCurrentFireModeId() : NAME_None;
	}
	else if (!IsCurrentWeaponHoldTrigger())
	{
		// Press 武器只响应按下瞬间，避免后续 Triggered 阶段连射。
		FireWeapon();
	}
}

void AWeaponCharacter::FireTriggered(const FInputActionValue& Value)
{
	#if !UE_BUILD_SHIPPING
	if (!bFireTriggeredLoggedThisPress)
	{
		bFireTriggeredLoggedThisPress = true;
		LogFireTraceState(TEXT("Enhanced first Triggered"));
	}
	#endif
	if (IsDead() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this)) return;

	if (ActiveFireInputSlot <= 1 && IsCurrentWeaponHoldTrigger())
	{
		// Hold 武器按住期间持续尝试激活，实际间隔仍由 GA_Fire 冷却控制。
		FireWeapon();
	}
	else if (ActiveFireInputSlot == 2)
	{
		ThrowGrenadeTriggered(Value);
	}
}

void AWeaponCharacter::FireCompleted(const FInputActionValue& Value)
{
	#if !UE_BUILD_SHIPPING
	LogFireTraceState(TEXT("Enhanced Completed"));
	#endif
	const bool bCanCompleteFire = !IsDead() && !AWeaponGameState::IsGameplayRecoveryBlockedFor(this);
	if (bCanCompleteFire && ActiveFireInputSlot <= 1 && bIsChargingFire)
	{
		UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>();
		const bool bSameSelection = EquipComp
			&& CurrentEquipmentSlot == ActiveFireInputSlot
			&& EquipComp->GetCurrentWeaponData() == ChargeWeaponData
			&& EquipComp->GetCurrentFireModeId() == ChargeFireModeId;
		const UGameDataSubsystem* DataSubsystem = GetGameInstance() ? GetGameInstance()->GetSubsystem<UGameDataSubsystem>() : nullptr;
		const FWeaponFireModeConfigRow* FireModeConfig = DataSubsystem ? DataSubsystem->GetWeaponFireModeConfig(ChargeFireModeId) : nullptr;
		const double Elapsed = GetWorld() ? GetWorld()->GetTimeSeconds() - ChargeFireStartTime : 0.0;
		if (bSameSelection && FireModeConfig && Elapsed >= FireModeConfig->ChargeDuration)
		{
			FireWeapon();
		}
	}
	else if (bCanCompleteFire && ActiveFireInputSlot == 2)
	{
		ThrowGrenadeCompleted(Value);
	}
	CancelChargeFire();
	ActiveFireInputSlot = 255;
	bFireTriggeredLoggedThisPress = false;
}

void AWeaponCharacter::FireCanceled(const FInputActionValue& Value)
{
	#if !UE_BUILD_SHIPPING
	LogFireTraceState(TEXT("Enhanced Canceled"));
	#endif
	if (!IsDead() && ActiveFireInputSlot == 2)
	{
		ThrowGrenadeCanceled(Value);
	}
	CancelChargeFire();
	ActiveFireInputSlot = 255;
	bFireTriggeredLoggedThisPress = false;
}

void AWeaponCharacter::RawFirePressed()
{
#if !UE_BUILD_SHIPPING
	LogFireTraceState(TEXT("Raw LeftMouse IE_Pressed"));
#endif
}

void AWeaponCharacter::RawFireReleased()
{
#if !UE_BUILD_SHIPPING
	LogFireTraceState(TEXT("Raw LeftMouse IE_Released"));
#endif
}

#if !UE_BUILD_SHIPPING
void AWeaponCharacter::LogFireTraceState(const TCHAR* EventName) const
{
	const UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>();
	const UWeaponDataAsset* WeaponData = EquipComp ? EquipComp->GetCurrentWeaponData() : nullptr;
	UE_LOG(LogTemp, Log, TEXT("[FireTrace] Input=%s ActiveFireInputSlot=%u CurrentEquipmentSlot=%u Weapon=%s Magazine=%d Reserve=%d Reloading=%s"),
		EventName,
		ActiveFireInputSlot,
		CurrentEquipmentSlot,
		*GetNameSafe(WeaponData),
		EquipComp ? EquipComp->GetCurrentMagazineAmmo() : -1,
		EquipComp ? EquipComp->GetCurrentReserveAmmo() : -1,
		EquipComp && EquipComp->IsReloading() ? TEXT("true") : TEXT("false"));
}
#endif

bool AWeaponCharacter::IsCurrentWeaponHoldTrigger() const
{
	const UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>();
	const UWeaponDataAsset* WeaponData = EquipComp ? EquipComp->GetCurrentWeaponData() : nullptr;
	if (!WeaponData || WeaponData->WeaponConfigId.IsNone())
	{
		return false;
	}

	const UGameInstance* GameInstance = GetGameInstance();
	const UGameDataSubsystem* DataSubsystem = GameInstance ? GameInstance->GetSubsystem<UGameDataSubsystem>() : nullptr;
	const FWeaponFireModeConfigRow* FireModeConfig = DataSubsystem
		? DataSubsystem->GetWeaponFireModeConfig(EquipComp->GetCurrentFireModeId())
		: nullptr;
	return FireModeConfig && FireModeConfig->TriggerType == FName(TEXT("Hold"));
}

bool AWeaponCharacter::IsCurrentWeaponChargeTrigger() const
{
	const UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>();
	const UGameDataSubsystem* DataSubsystem = GetGameInstance() ? GetGameInstance()->GetSubsystem<UGameDataSubsystem>() : nullptr;
	const FWeaponFireModeConfigRow* FireModeConfig = EquipComp && DataSubsystem
		? DataSubsystem->GetWeaponFireModeConfig(EquipComp->GetCurrentFireModeId())
		: nullptr;
	return FireModeConfig && FireModeConfig->TriggerType == FName(TEXT("Charge"));
}

void AWeaponCharacter::CancelChargeFire()
{
	bIsChargingFire = false;
	ChargeFireStartTime = 0.0;
	ChargeWeaponData = nullptr;
	ChargeFireModeId = NAME_None;
}

float AWeaponCharacter::GetChargeFireProgress() const
{
	if (!bIsChargingFire || !GetWorld())
	{
		return 0.0f;
	}

	const UGameInstance* GameInstance = GetGameInstance();
	const UGameDataSubsystem* DataSubsystem = GameInstance ? GameInstance->GetSubsystem<UGameDataSubsystem>() : nullptr;
	const FWeaponFireModeConfigRow* FireModeConfig = DataSubsystem ? DataSubsystem->GetWeaponFireModeConfig(ChargeFireModeId) : nullptr;
	if (!FireModeConfig || FireModeConfig->ChargeDuration <= 0.0f)
	{
		return 0.0f;
	}

	const double Elapsed = GetWorld()->GetTimeSeconds() - ChargeFireStartTime;
	return FMath::Clamp(static_cast<float>(Elapsed / FireModeConfig->ChargeDuration), 0.0f, 1.0f);
}

void AWeaponCharacter::CycleFireMode()
{
	if (IsDead() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this) || CurrentEquipmentSlot > 1)
	{
		return;
	}
	CancelChargeFire();
	if (UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>())
	{
		EquipComp->CycleFireMode();
	}
	if (!HasAuthority())
	{
		ServerCycleFireMode();
	}
}

void AWeaponCharacter::ServerCycleFireMode_Implementation()
{
	if (!IsDead() && !AWeaponGameState::IsGameplayRecoveryBlockedFor(this) && CurrentEquipmentSlot <= 1)
	{
		if (UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>())
		{
			EquipComp->CycleFireMode();
		}
	}
}

void AWeaponCharacter::FireWeapon()
{
	if (AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}
	// 通过 ASC 尝试激活射击 Ability（由 WeaponEquipmentComponent 授予）
	if (!AbilitySystemComponent)
	{
		#if !UE_BUILD_SHIPPING
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] FireWeapon input arrived but ASC is null"));
		#endif
		return;
	}

	UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>();
	if (EquipComp && EquipComp->GetCurrentWeaponData() && EquipComp->GetCurrentWeaponData()->FireAbilityClass)
	{
		const UWeaponDataAsset* WeaponData = EquipComp->GetCurrentWeaponData();
		const bool bActivated = AbilitySystemComponent->TryActivateAbilityByClass(WeaponData->FireAbilityClass);

		// 激活成功且本地控制时，触发相机后坐力反馈
		if (bActivated && IsLocallyControlled())
		{
			if (UGameInstance* GI = GetGameInstance())
			{
				if (UGameDataSubsystem* DataSubsystem = GI->GetSubsystem<UGameDataSubsystem>())
				{
					const FWeaponFireModeConfigRow* FireModeConfig = DataSubsystem->GetWeaponFireModeConfig(EquipComp->GetCurrentFireModeId());
					if (FireModeConfig && FireModeConfig->Recoil > 0.0f)
					{
						// 每点 Recoil 拉近约 100 UU，最大叠加 180 UU
						const float RecoilImpulse = FireModeConfig->Recoil * 100.0f;
						CurrentRecoilOffset = FMath::Min(CurrentRecoilOffset + RecoilImpulse, 180.0f);
						SpringArm->TargetArmLength = BaseTargetArmLength - CurrentRecoilOffset;
					}
				}
			}
		}

		#if !UE_BUILD_SHIPPING
		if (bActivated)
		{
			UE_LOG(LogTemp, Log, TEXT("[FireTrace] FireWeapon input arrived GAS=accepted Cooldown.Weapon.Fire=%s Weapon=%s"),
				AbilitySystemComponent->HasMatchingGameplayTag(TAG_Cooldown_Weapon_Fire) ? TEXT("true") : TEXT("false"),
				*GetNameSafe(WeaponData));
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[FireTrace] FireWeapon input arrived GAS=rejected Cooldown.Weapon.Fire=%s Weapon=%s"),
				AbilitySystemComponent->HasMatchingGameplayTag(TAG_Cooldown_Weapon_Fire) ? TEXT("true") : TEXT("false"),
				*GetNameSafe(WeaponData));
		}
		#endif
	}
	#if !UE_BUILD_SHIPPING
	else
	{
		UE_LOG(LogTemp, Warning, TEXT("[FireTrace] FireWeapon input arrived but weapon/equipment/FireAbilityClass is missing Weapon=%s"),
			*GetNameSafe(EquipComp ? EquipComp->GetCurrentWeaponData() : nullptr));
	}
	#endif
}

void AWeaponCharacter::Melee(const FInputActionValue& Value)
{
	// 死亡时禁止近战
	if (IsDead() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
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

void AWeaponCharacter::ReloadWeapon()
{
	if (IsDead() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}

	if (UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>())
	{
		if (EquipComp->GetCurrentWeaponData())
		{
			EquipComp->StartReload();
		}
	}
	if (!HasAuthority())
	{
		ServerReloadWeapon();
	}
}

void AWeaponCharacter::ServerReloadWeapon_Implementation()
{
	if (IsDead() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}

	if (UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>())
	{
		if (EquipComp->GetCurrentWeaponData())
		{
			EquipComp->StartReload();
		}
	}
}

void AWeaponCharacter::SwitchWeapon(const FInputActionValue& Value)
{
	CancelChargeFire();
	// 死亡时禁止切枪
	if (IsDead() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
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
	const int32 WeaponCount = EquipComp->GetWeaponInventory().Num();
	if (WeaponCount <= 0 || FMath::IsNearlyZero(ScrollValue))
	{
		return;
	}
	const int32 BaseIndex = CurrentEquipmentSlot < WeaponCount ? CurrentEquipmentSlot : 0;
	const int32 NewIndex = ScrollValue > 0.0f
		? (BaseIndex + 1) % WeaponCount
		: (BaseIndex - 1 + WeaponCount) % WeaponCount;
	SelectEquipmentSlot(static_cast<uint8>(NewIndex));
}

void AWeaponCharacter::SelectPrimary()
{
	const AWeaponPlayerState* PS = GetPlayerState<AWeaponPlayerState>();
	const UWeaponHealthSet* HealthSet = PS ? PS->GetHealthSet() : nullptr;
	const UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>();
	UE_LOG(LogTemp, Warning,
		TEXT("[DeathTrace] Key1 pressed: Character=%s Health=%.2f Shield=%.2f Dead=%d Slot=%u WeaponIndex=%d Weapon=%s"),
		*GetNameSafe(this),
		HealthSet ? HealthSet->GetHealth() : -1.0f,
		HealthSet ? HealthSet->GetShield() : -1.0f,
		IsDead() ? 1 : 0,
		CurrentEquipmentSlot,
		EquipComp ? EquipComp->GetCurrentWeaponIndex() : INDEX_NONE,
		*GetNameSafe(EquipComp ? EquipComp->GetCurrentWeaponData() : nullptr));

	SelectEquipmentSlot(0);

	UE_LOG(LogTemp, Warning,
		TEXT("[DeathTrace] Key1 completed: Character=%s Health=%.2f Shield=%.2f Dead=%d Hidden=%d Slot=%u WeaponIndex=%d Weapon=%s"),
		*GetNameSafe(this),
		HealthSet ? HealthSet->GetHealth() : -1.0f,
		HealthSet ? HealthSet->GetShield() : -1.0f,
		IsDead() ? 1 : 0,
		IsHidden() ? 1 : 0,
		CurrentEquipmentSlot,
		EquipComp ? EquipComp->GetCurrentWeaponIndex() : INDEX_NONE,
		*GetNameSafe(EquipComp ? EquipComp->GetCurrentWeaponData() : nullptr));
}

void AWeaponCharacter::SelectSecondary()
{
	SelectEquipmentSlot(1);
}

void AWeaponCharacter::SelectSkill()
{
	SelectEquipmentSlot(2);
}

void AWeaponCharacter::SelectTool()
{
	SelectEquipmentSlot(3);
}

void AWeaponCharacter::SelectEquipmentSlot(uint8 SlotIndex)
{
	if (IsDead() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this)) return;
	if (!HasAuthority())
	{
		ServerSelectEquipmentSlot(SlotIndex);
		return;
	}
	if (SlotIndex > 3) return;
	if (SlotIndex == 2 && !bHasSkillItem) return;
	if (SlotIndex == 3 && !bHasProfessionTool) return;
	CancelChargeFire();

	// 按住手雷时切槽视为取消，避免松开左键后从其它槽意外投掷。
	if (ActiveFireInputSlot == 2)
	{
		bIsPreviewingGrenade = false;
		PreviewGrenadeTarget = FVector::ZeroVector;
		bHasPreviewGrenadeTarget = false;
		HideGrenadePreview();
		ActiveFireInputSlot = 255;
	}

	UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>();
	if (!EquipComp) return;
	if (SlotIndex <= 1 && !EquipComp->GetWeaponInventory().IsValidIndex(SlotIndex)) return;

	CurrentEquipmentSlot = SlotIndex;
	if (SlotIndex <= 1)
	{
		EquipComp->SwitchWeapon(SlotIndex);
	}
	else
	{
		EquipComp->SelectNonWeaponSlot();
	}
	ForceNetUpdate();
}

void AWeaponCharacter::ServerSelectEquipmentSlot_Implementation(uint8 SlotIndex)
{
	SelectEquipmentSlot(SlotIndex);
}

void AWeaponCharacter::OnRep_CurrentEquipmentSlot()
{
	ActiveFireInputSlot = 255;
}

bool AWeaponCharacter::RestoreEquipmentSlot(uint8 SlotIndex)
{
	if (!HasAuthority() || SlotIndex > 3)
	{
		return false;
	}
	UWeaponEquipmentComponent* EquipComp = FindComponentByClass<UWeaponEquipmentComponent>();
	if (!EquipComp || (SlotIndex == 2 && !bHasSkillItem) ||
		(SlotIndex == 3 && !bHasProfessionTool))
	{
		return false;
	}
	if (SlotIndex <= 1)
	{
		if (!EquipComp->RestoreWeaponSelection(SlotIndex))
		{
			return false;
		}
	}
	else
	{
		EquipComp->SelectNonWeaponSlot();
	}
	CurrentEquipmentSlot = SlotIndex;
	ForceNetUpdate();
	return CurrentEquipmentSlot == SlotIndex;
}

void AWeaponCharacter::DeployProfessionTool()
{
	if (IsDead() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this) ||
		!bHasProfessionTool || !ProfessionToolDroneClass || IsValid(ActiveDrone)) return;

	if (!HasAuthority())
	{
		ServerDeployProfessionTool();
		return;
	}

	FActorSpawnParameters SpawnParams;
	SpawnParams.Owner = this;
	ActiveDrone = GetWorld()->SpawnActor<ADrone>(ProfessionToolDroneClass, GetActorLocation(), GetActorRotation(), SpawnParams);
	if (ActiveDrone)
	{
		ActiveDrone->SetOrbitTarget(this);
	}
}

void AWeaponCharacter::ServerDeployProfessionTool_Implementation()
{
	DeployProfessionTool();
}

// ============================================================================
// 手雷投掷：预览 + ServerRPC 流程
// ============================================================================

void AWeaponCharacter::ThrowGrenadeStarted(const FInputActionValue& Value)
{
	// 死亡时禁止投掷
	if (IsDead() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
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
	if (AWeaponGameState::IsGameplayRecoveryBlockedFor(this) ||
		!bEnableGrenadePreview || !bIsPreviewingGrenade)
	{
		return;
	}

	UpdateGrenadePreview();
}

void AWeaponCharacter::ThrowGrenadeCompleted(const FInputActionValue& Value)
{
	if (AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		bIsPreviewingGrenade = false;
		HideGrenadePreview();
		return;
	}
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

void AWeaponCharacter::QuickThrowGrenadePressed()
{
	// 3 号槽 + 左键的投掷进行中时不再叠一次快速投掷;无手雷职业直接忽略(服务器仍会再校验)。
	if (IsDead() || !bHasSkillItem || ActiveFireInputSlot == 2 || bQuickGrenadeHeld)
	{
		return;
	}
	bQuickGrenadeHeld = true;
	ThrowGrenadeStarted(FInputActionValue());
}

void AWeaponCharacter::QuickThrowGrenadeReleased()
{
	if (!bQuickGrenadeHeld)
	{
		return;
	}
	bQuickGrenadeHeld = false;
	if (IsDead())
	{
		ThrowGrenadeCanceled(FInputActionValue());
		return;
	}
	ThrowGrenadeCompleted(FInputActionValue());
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
	if (IsDead() || AWeaponGameState::IsGameplayRecoveryBlockedFor(this) || !bHasSkillItem)
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

void AWeaponCharacter::HandleGameplayCue(AActor* Self, FGameplayTag GameplayCueTag, EGameplayCueEvent::Type EventType, const FGameplayCueParameters& Parameters)
{
	WeaponGameplayCues::HandleCue(Self, GameplayCueTag, EventType, Parameters);
}
