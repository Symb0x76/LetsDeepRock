// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Containers/Ticker.h"
#include "Online/WeaponHostMigrationTypes.h"
#include "Online/WeaponSessionSubsystem.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "WeaponHostMigrationHarnessSubsystem.generated.h"

/**
 * Opt-in packaged-process driver for the Host Migration acceptance harness.
 *
 * This subsystem is never created in Shipping and is inert unless
 * -WeaponMigrationHarnessRole=Host|Client1|Client2 is present. It drives only the public
 * Session APIs and observes the production migration state machine; it does not skip protocol
 * phases or mutate recovery state directly.
 */
UCLASS()
class UWeaponHostMigrationHarnessSubsystem final : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

private:
	enum class EProcessRole : uint8
	{
		None,
		Host,
		Client,
	};

	UFUNCTION()
	void HandleHostComplete(bool bWasSuccessful, FString Message);

	UFUNCTION()
	void HandleFindComplete(bool bWasSuccessful, const TArray<FWeaponLanSessionResult>& Results);

	UFUNCTION()
	void HandleJoinComplete(bool bWasSuccessful, FString Message);

	bool TickHarness(float DeltaSeconds);
	void DriveInitialSession();
	void ObserveMigration();
	void EmitMarker(const TCHAR* Event, const FString& Details = FString()) const;

	static const TCHAR* LexMigrationState(EWeaponHostMigrationState State);
	static const TCHAR* LexMigrationRole(EWeaponMigrationRole Role);
	static const TCHAR* LexNetMode(ENetMode NetMode);
	static FString SanitizeMarkerValue(FString Value);

	EProcessRole ProcessRole = EProcessRole::None;
	FString ProcessLabel;
	FString RunId;
	FString ServerName;
	FTSTicker::FDelegateHandle TickerHandle;
	double NextFindAttemptSeconds = 0.0;
	EWeaponHostMigrationState LastMigrationState = EWeaponHostMigrationState::Idle;
	bool bHostRequestStarted = false;
	bool bFindInFlight = false;
	bool bInitialJoinStarted = false;
	bool bInitialConnectionObserved = false;
	bool bBaselineEmitted = false;
	bool bSawRecoveryTransition = false;
	bool bRecoveryLiveEmitted = false;
};
