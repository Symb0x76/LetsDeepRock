// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Character.h"
#include "LetsDeepRockCharacter.generated.h"

class UCameraComponent;
class USpringArmComponent;
class UInputMappingContext;
class UInputAction;
struct FInputActionValue;

/**
 *  A controllable top-down perspective character.
 *  Uses WASD (Enhanced Input) for movement.
 */
UCLASS(abstract)
class ALetsDeepRockCharacter : public ACharacter
{
	GENERATED_BODY()

public:
	/** Constructor */
	ALetsDeepRockCharacter();

	/** Initialization */
	virtual void BeginPlay() override;

	/** Update */
	virtual void Tick(float DeltaSeconds) override;

	/** Set up Enhanced Input action bindings */
	virtual void SetupPlayerInputComponent(class UInputComponent* PlayerInputComponent) override;
	
	/** Meta Replication **/
	virtual void GetLifetimeReplicatedProps(TArray<class FLifetimeProperty>& OutLifetimeProps) const override;
	
protected:
	/** Handles WASD movement input */
	void Move(const FInputActionValue& Value);

	/** Returns the camera component **/
	UCameraComponent* GetTopDownCameraComponent() const { return TopDownCameraComponent.Get(); }

	/** Returns the Camera Boom component **/
	USpringArmComponent* GetCameraBoom() const { return CameraBoom.Get(); }

	/** Reply Health Update **/
	void OnHealthUpdate() const;
	
public:
	UFUNCTION(BlueprintPure, Category = "Health")
	FORCEINLINE float GetMaxHealth() const { return MaxHealth; }
	
	UFUNCTION(BlueprintPure, Category = "Health")
	FORCEINLINE float GetCurrentHealth() const { return CurrentHealth; }
	
	/** 当前生命值的存值函数。范围限定在0到MaxHealth之间，并调用OnHealthUpdate。仅在服务器上调用。**/
	UFUNCTION(BlueprintCallable, Category = "Health")
	void SetCurrentHealth(float HealthValue);
	
	UFUNCTION(BlueprintCallable, Category = "Health")
	virtual float TakeDamage(float DamageAmount, struct FDamageEvent const& DamageEvent, class AController* EventInstigator, AActor* DamageCauser) override;	
	
protected:

	/** Input mapping context applied to the owning player when possessed */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputMappingContext> DefaultMappingContext;

	/** Movement input action (Axis2D). Map W/A/S/D to this in the IMC. */
	UPROPERTY(EditAnywhere, BlueprintReadOnly, Category="Input", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UInputAction> MoveAction;

	/** Player Max HP **/
	UPROPERTY(EditDefaultsOnly, Category = "Health")
	float MaxHealth;
	
	/** Player current HP **/
	UPROPERTY(ReplicatedUsing = OnRep_CurrentHealth)	// enable server-side copy of Actor
	float CurrentHealth;
	
	/** RepNotify. Sync HP changes **/
	UFUNCTION()
	void OnRep_CurrentHealth() const;

private:
	/** Top down camera */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<UCameraComponent> TopDownCameraComponent;

	/** Camera boom positioning the camera above the character */
	UPROPERTY(VisibleAnywhere, BlueprintReadOnly, Category="Components", meta = (AllowPrivateAccess = "true"))
	TObjectPtr<USpringArmComponent> CameraBoom;
};

