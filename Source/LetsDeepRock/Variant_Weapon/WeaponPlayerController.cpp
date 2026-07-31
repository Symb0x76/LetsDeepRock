// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponPlayerController.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/GameInstance.h"
#include "InputMappingContext.h"
#include "LetsDeepRock.h"
#include "Online/WeaponHostMigrationSubsystem.h"
#include "WeaponGameMode.h"

AWeaponPlayerController::AWeaponPlayerController()
{
	// 俯视角鼠标瞄准：显示鼠标光标
	bShowMouseCursor = true;
	DefaultMouseCursor = EMouseCursor::Crosshairs;

	// 鼠标输入直接反映到光标位置（不影响 Controller Rotation）
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;
}

void AWeaponPlayerController::SetupInputComponent()
{
	Super::SetupInputComponent();

	// 注册默认的输入映射上下文
	if (UEnhancedInputLocalPlayerSubsystem* Subsystem =
		ULocalPlayer::GetSubsystem<UEnhancedInputLocalPlayerSubsystem>(GetLocalPlayer()))
	{
		for (UInputMappingContext* CurrentContext : DefaultMappingContexts)
		{
			if (CurrentContext)
			{
				Subsystem->AddMappingContext(CurrentContext, 0);
			}
		}
	}
}

void AWeaponPlayerController::PawnLeavingGame()
{
	if (AWeaponGameMode* WeaponGameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AWeaponGameMode>() : nullptr;
		WeaponGameMode && WeaponGameMode->PreserveLeavingPlayerAttribution(this))
	{
		return;
	}
	Super::PawnLeavingGame();
}

void AWeaponPlayerController::ClientReceiveMigrationPayloadChunk_Implementation(
	const FWeaponMigrationPayloadEnvelope& Envelope,
	const TArray<uint8>& ChunkBytes)
{
	UWeaponHostMigrationSubsystem* MigrationSubsystem =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>() : nullptr;
	if (!MigrationSubsystem)
	{
		UE_LOG(LogLetsDeepRock, Error, TEXT("Migration chunk received without HostMigrationSubsystem"));
		return;
	}

	bool bAckReady = false;
	FWeaponMigrationPayloadAck Ack;
	FString Error;
	if (!MigrationSubsystem->ReceivePayloadChunk(
		Envelope,
		ChunkBytes,
		bAckReady,
		Ack,
		Error))
	{
		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("Migration chunk rejected for revision %lld index %d: %s"),
			Envelope.SnapshotRevision,
			Envelope.ChunkIndex,
			*Error);
		return;
	}

	if (bAckReady)
	{
		ServerAcknowledgeMigrationPayload(Ack);
	}
}

void AWeaponPlayerController::ServerAcknowledgeMigrationPayload_Implementation(
	const FWeaponMigrationPayloadAck& Ack)
{
	UWeaponHostMigrationSubsystem* MigrationSubsystem =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>() : nullptr;
	if (!MigrationSubsystem)
	{
		UE_LOG(LogLetsDeepRock, Error, TEXT("Migration ACK received without HostMigrationSubsystem"));
		return;
	}

	FString Error;
	if (!MigrationSubsystem->RecordBackupAcknowledgement(this, Ack, Error))
	{
		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("Migration ACK rejected for revision %lld: %s"),
			Ack.SnapshotRevision,
			*Error);
	}
}
