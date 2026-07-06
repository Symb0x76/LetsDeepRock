// Copyright Epic Games, Inc. All Rights Reserved.

#include "LetsDeepRockCharacter.h"
#include "UObject/ConstructorHelpers.h"
#include "Camera/CameraComponent.h"
#include "Components/CapsuleComponent.h"
#include "GameFramework/CharacterMovementComponent.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/SpringArmComponent.h"
#include "Net/UnrealNetwork.h"
#include "Engine/Engine.h"
#include "Materials/Material.h"
#include "Engine/World.h"
#include "Engine/LocalPlayer.h"
#include "EnhancedInputComponent.h"
#include "EnhancedInputSubsystems.h"
#include "InputActionValue.h"
#include "LetsDeepRock.h"

ALetsDeepRockCharacter::ALetsDeepRockCharacter()
{
	// Set size for player capsule
	GetCapsuleComponent()->InitCapsuleSize(42.f, 96.0f);

	// Don't rotate character to camera direction
	bUseControllerRotationPitch = false;
	bUseControllerRotationYaw = false;
	bUseControllerRotationRoll = false;

	// Configure character movement
	GetCharacterMovement()->bOrientRotationToMovement = true;
	GetCharacterMovement()->RotationRate = FRotator(0.f, 640.f, 0.f);
	GetCharacterMovement()->bConstrainToPlane = true;
	GetCharacterMovement()->bSnapToPlaneAtStart = true;

	// Create the camera boom component
	CameraBoom = CreateDefaultSubobject<USpringArmComponent>(TEXT("CameraBoom"));

	CameraBoom->SetupAttachment(RootComponent);
	CameraBoom->SetUsingAbsoluteRotation(true);
	CameraBoom->TargetArmLength = 800.f;
	CameraBoom->SetRelativeRotation(FRotator(-60.f, 0.f, 0.f));
	CameraBoom->bDoCollisionTest = false;

	// Create the camera component
	TopDownCameraComponent = CreateDefaultSubobject<UCameraComponent>(TEXT("TopDownCamera"));

	TopDownCameraComponent->SetupAttachment(CameraBoom, USpringArmComponent::SocketName);
	TopDownCameraComponent->bUsePawnControlRotation = false;

	// Activate ticking in order to update the cursor every frame.
	PrimaryActorTick.bCanEverTick = true;
	PrimaryActorTick.bStartWithTickEnabled = true;
	
	// Init Player HP
	MaxHealth = 100.0f;
	CurrentHealth = MaxHealth;
}

void ALetsDeepRockCharacter::BeginPlay()
{
	Super::BeginPlay();

	// Register the default input mapping context for the owning local player
	if (APlayerController* PlayerController = Cast<APlayerController>(GetController()))
	{
		if (UEnhancedInputLocalPlayerSubsystem* Subsystem = ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(PlayerController->GetLocalPlayer()))
		{
			if (DefaultMappingContext)
			{
				Subsystem->AddMappingContext(DefaultMappingContext, 0);
			}
		}
	}
}

void ALetsDeepRockCharacter::Tick(float DeltaSeconds)
{
    Super::Tick(DeltaSeconds);

	// stub
}

void ALetsDeepRockCharacter::SetupPlayerInputComponent(UInputComponent* PlayerInputComponent)
{
	Super::SetupPlayerInputComponent(PlayerInputComponent);

	if (UEnhancedInputComponent* EnhancedInputComponent = Cast<UEnhancedInputComponent>(PlayerInputComponent))
	{
		// Bind WASD movement
		EnhancedInputComponent->BindAction(MoveAction, ETriggerEvent::Triggered, this, &ALetsDeepRockCharacter::Move);
	}
	else
	{
		UE_LOG(LogLetsDeepRock, Error, TEXT("'%s' Failed to find an Enhanced Input Component! This template is built to use the Enhanced Input system."), *GetNameSafe(this));
	}
}

void ALetsDeepRockCharacter::GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	
	// Replicate current HP
	DOREPLIFETIME(ALetsDeepRockCharacter, CurrentHealth);
}

void ALetsDeepRockCharacter::Move(const FInputActionValue& Value)
{
	// Input is a Vector2D (X = right/left, Y = forward/back)
	const FVector2D MovementVector = Value.Get<FVector2D>();

	if (Controller != nullptr)
	{
		// Move relative to world axes so WASD maps to the fixed top-down camera view.
		// Yaw-only rotation keeps movement on the ground plane.
		const FRotator YawRotation(0.f, Controller->GetControlRotation().Yaw, 0.f);

		const FVector ForwardDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::X);
		const FVector RightDirection = FRotationMatrix(YawRotation).GetUnitAxis(EAxis::Y);

		AddMovementInput(ForwardDirection, MovementVector.Y);
		AddMovementInput(RightDirection, MovementVector.X);
	}
}

void ALetsDeepRockCharacter::OnHealthUpdate()
{
	// client
	if (IsLocallyControlled())
	{
		const FString HealthMessage = FString::Printf(TEXT("Current Health: %f"), CurrentHealth);
		GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Green, HealthMessage);
		
		if (CurrentHealth <= 0.0f)
		{
			const FString DeathMessage = FString::Printf(TEXT("Player has died!"));
			GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, DeathMessage);
		}
	}
	
	// server
	if (GetLocalRole())
	{
		const FString HealthMessage = FString::Printf(TEXT("%s has %f health remaining."), *GetFName().ToString(), CurrentHealth);
		GEngine->AddOnScreenDebugMessage(-1, 5.f, FColor::Red, HealthMessage);
	}
	
	// all
	/*
	 * Any health-related functions here
	 */
}

void ALetsDeepRockCharacter::OnRep_CurrentHealth()
{
	OnHealthUpdate();
}
