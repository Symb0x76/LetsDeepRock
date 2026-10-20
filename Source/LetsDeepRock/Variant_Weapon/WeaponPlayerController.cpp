// Copyright Epic Games, Inc. All Rights Reserved.


#include "WeaponPlayerController.h"
#include "EnhancedInputSubsystems.h"
#include "Engine/GameInstance.h"
#include "GameFramework/PlayerState.h"
#include "InputMappingContext.h"
#include "LetsDeepRock.h"
#include "Online/WeaponHostMigrationSubsystem.h"
#include "TimerManager.h"
#include "InputCoreTypes.h"
#include "UI/WeaponDebugTypes.h"
#include "UI/WeaponHUD.h"
#include "UI/WeaponClassSelectWidget.h"
#include "UI/WeaponCombatHUDWidget.h"
#include "UI/WeaponLobbyWidget.h"
#include "WeaponGameMode.h"
#include "WeaponPlayerState.h"
#include "Data/ProfessionSaveGame.h"
#include "Blueprint/UserWidget.h"

namespace
{
	bool IsMigrationTravelInProgress(const UWeaponHostMigrationSubsystem* MigrationSubsystem)
	{
		if (!MigrationSubsystem)
		{
			return false;
		}

		const EWeaponHostMigrationState State = MigrationSubsystem->GetMigrationState();
		return State != EWeaponHostMigrationState::Idle &&
			State != EWeaponHostMigrationState::Monitoring;
	}
}

AWeaponPlayerController::AWeaponPlayerController()
{
	// 俯视角鼠标瞄准：显示鼠标光标
	bShowMouseCursor = true;
	DefaultMouseCursor = EMouseCursor::Crosshairs;

	// 鼠标输入直接反映到光标位置（不影响 Controller Rotation）
	bEnableClickEvents = true;
	bEnableMouseOverEvents = true;

	// 默认职业选择 UI 类（可由蓝图子类替换）
	ClassSelectWidgetClass = UWeaponClassSelectWidget::StaticClass();

	// 默认房间 UI 类（可由蓝图子类替换）
	LobbyWidgetClass = UWeaponLobbyWidget::StaticClass();
	CombatHUDWidgetClass = UWeaponCombatHUDWidget::StaticClass();
}

void AWeaponPlayerController::BeginPlay()
{
	Super::BeginPlay();

	// 关卡内自动显示职业选择 UI：仅本地控制的 PlayerController 需要 UI
	if (IsLocalController())
	{
		ApplyGameplayInputMode();

		// 房间 UI 先于职业选择创建，保证职业选择模态层始终盖在它上面。
		ShowLobbyUI();

		// 战斗 HUD(A3):纯展示层,压在房间 UI 之下,自身按 InGame 阶段显隐。
		if (CombatHUDWidgetClass && !CombatHUDWidget)
		{
			CombatHUDWidget = CreateWidget<UWeaponCombatHUDWidget>(this, CombatHUDWidgetClass);
			if (CombatHUDWidget)
			{
				CombatHUDWidget->AddToViewport(5);
			}
		}

		const UWeaponHostMigrationSubsystem* MigrationSubsystem =
			GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>() : nullptr;
		if (!IsMigrationTravelInProgress(MigrationSubsystem))
		{
			ShowClassSelectUI();
		}
	}
}

void AWeaponPlayerController::ShowLobbyUI()
{
	if (!LobbyWidgetClass || LobbyWidget)
	{
		return;
	}

	LobbyWidget = CreateWidget<UWeaponLobbyWidget>(this, LobbyWidgetClass);
	if (LobbyWidget)
	{
		// 房间 UI 常驻显示，默认是屏幕正左的小窗；不改变输入模式，
		// 玩法输入仍由 ApplyGameplayInputMode 的 GameAndUI 负责。
		// ZOrder 10 保证在职业选择 UI（ZOrder 100）之下。
		LobbyWidget->AddToViewport(10);
	}
}

void AWeaponPlayerController::ShowClassSelectUI()
{
	if (!ClassSelectWidgetClass || ClassSelectWidget)
	{
		return;
	}

	ClassSelectWidget = CreateWidget<UWeaponClassSelectWidget>(this, ClassSelectWidgetClass);
	if (ClassSelectWidget)
	{
		// ZOrder 100 保证职业选择在房间 UI（ZOrder 10）之上。
		ClassSelectWidget->AddToViewport(100);

		// 职业选择期间隐藏房间 UI，避免信息冗余和交互干扰。
		if (LobbyWidget)
		{
			LobbyWidget->SetVisibility(ESlateVisibility::Hidden);
		}

		// 职业选择是模态界面，选择完成前只接收 UI 输入。
		SetInputMode(FInputModeUIOnly().SetWidgetToFocus(ClassSelectWidget->TakeWidget()));
		bShowMouseCursor = true;
	}
}

void AWeaponPlayerController::ApplyGameplayInputMode()
{
	FInputModeGameAndUI InputMode;
	InputMode.SetHideCursorDuringCapture(false);
	InputMode.SetLockMouseToViewportBehavior(EMouseLockMode::LockAlways);
	SetInputMode(InputMode);
	bShowMouseCursor = true;
}

void AWeaponPlayerController::RestoreGameplayInputAfterMigration()
{
	if (!IsLocalController())
	{
		return;
	}

	if (ClassSelectWidget)
	{
		ClassSelectWidget->RemoveFromParent();
		ClassSelectWidget = nullptr;
	}

	// 迁移后恢复房间 UI 的可见性
	if (LobbyWidget)
	{
		LobbyWidget->SetVisibility(ESlateVisibility::Visible);
	}

	ApplyGameplayInputMode();
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

#if !UE_BUILD_SHIPPING
	InputComponent->BindKey(EKeys::F8, IE_Pressed, this, &AWeaponPlayerController::ToggleWeaponDebugUI);
#endif
}

void AWeaponPlayerController::ToggleWeaponDebugUI()
{
	SetWeaponDebugUIEnabled(!IsWeaponDebugUIEnabled());
}

void AWeaponPlayerController::ClientReceiveWeaponDamageDebugEvent_Implementation(const FWeaponDamageDebugEvent& Event)
{
#if !UE_BUILD_SHIPPING
	if (IsWeaponDebugUIEnabled())
	{
		if (AWeaponHUD* WeaponHUD = GetHUD<AWeaponHUD>())
		{
			WeaponHUD->AddWeaponDamageDebugEvent(Event);
		}
	}
#endif
}

void AWeaponPlayerController::RequestSelectProfession(FName ProfessionId, FName PrimaryWeaponId, FName SecondaryWeaponId)
{
	// 本地先完成同一份配置校验与保存；失败时保留界面，允许玩家重新选择。
	if (!UProfessionSaveGame::SelectProfession(this, ProfessionId, PrimaryWeaponId, SecondaryWeaponId))
	{
		return;
	}

	// 本地立即隐藏职业选择 UI，避免等待服务端往返造成的操作滞后感；
	// 服务端仍会重复校验并执行权威重生。
	if (ClassSelectWidget)
	{
		ClassSelectWidget->RemoveFromParent();
		ClassSelectWidget = nullptr;

		// 恢复房间 UI 的可见性（职业选择期间被隐藏）
		if (LobbyWidget)
		{
			LobbyWidget->SetVisibility(ESlateVisibility::Visible);
		}

		ApplyGameplayInputMode();
	}

	ServerSelectProfessionAndRespawn(ProfessionId, PrimaryWeaponId, SecondaryWeaponId);
}

void AWeaponPlayerController::ServerSelectProfessionAndRespawn_Implementation(FName ProfessionId, FName PrimaryWeaponId, FName SecondaryWeaponId)
{
	AWeaponPlayerState* WeaponPlayerState = GetPlayerState<AWeaponPlayerState>();
	if (!WeaponPlayerState || !WeaponPlayerState->SetProfessionLoadout(ProfessionId, PrimaryWeaponId, SecondaryWeaponId))
	{
		return;
	}

	if (AWeaponGameMode* GameMode = GetWorld() ? GetWorld()->GetAuthGameMode<AWeaponGameMode>() : nullptr)
	{
		GameMode->RespawnPlayerImmediately(this);
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
	const FUniqueNetIdRepl& ReceiverPlayerId,
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
		ReceiverPlayerId,
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

void AWeaponPlayerController::ClientUpdateMigrationRoleAssignment_Implementation(
	const FWeaponMigrationRoleAssignment& Assignment)
{
	UWeaponHostMigrationSubsystem* MigrationSubsystem =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>() : nullptr;
	FString Error;
	if (!MigrationSubsystem || !MigrationSubsystem->AcceptRoleAssignment(Assignment, Error))
	{
		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("Migration role assignment rejected for Epoch %llu: %s"),
			Assignment.AuthorityEpoch,
			MigrationSubsystem ? *Error : TEXT("HostMigrationSubsystem is unavailable"));
	}
}

void AWeaponPlayerController::ClientConfirmMigrationCommonCommitted_Implementation(
	const FUniqueNetIdRepl& ReceiverPlayerId,
	const FWeaponMigrationPayloadAck& Ack)
{
	UWeaponHostMigrationSubsystem* MigrationSubsystem =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>() : nullptr;
	FString Error;
	if (!MigrationSubsystem ||
		!MigrationSubsystem->AcceptCommonCommittedAnnouncement(ReceiverPlayerId, Ack, Error))
	{
		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("CommonCommitted announcement rejected for revision %lld: %s"),
			Ack.SnapshotRevision,
			MigrationSubsystem ? *Error : TEXT("HostMigrationSubsystem is unavailable"));
		return;
	}

	// Every survivor needs the CommonCommitted metadata for recovery discovery, but only
	// the two fenced backups participate in the convergence confirmation handshake.
	const EWeaponMigrationRole ReceiverRole = MigrationSubsystem->GetRoleForPlayer(ReceiverPlayerId);
	if (ReceiverRole == EWeaponMigrationRole::BackupA ||
		ReceiverRole == EWeaponMigrationRole::BackupB)
	{
		ServerConfirmMigrationCommonCommitted(Ack);
	}
}

void AWeaponPlayerController::ClientRequestRecoveryReadyAcknowledgement_Implementation(
	const FGuid MatchId,
	const uint64 AuthorityEpoch,
	const uint64 RosterRevision,
	const FGuid RecoveryAttemptId)
{
	UWeaponHostMigrationSubsystem* MigrationSubsystem =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>() : nullptr;
	FString Error;
	if (!MigrationSubsystem || !MigrationSubsystem->AcceptRecoveryReadyRequest(
		MatchId,
		AuthorityEpoch,
		RosterRevision,
		RecoveryAttemptId,
		Error))
	{
		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("RecoveryReady request rejected for Match %s Epoch %llu Roster %llu Attempt %s: %s"),
			*MatchId.ToString(),
			AuthorityEpoch,
			RosterRevision,
			*RecoveryAttemptId.ToString(),
			MigrationSubsystem ? *Error : TEXT("HostMigrationSubsystem is unavailable"));
		return;
	}

	// RecoveryReady means this travel is resuming an existing match. Clear any modal
	// class-selection input left by PlayerController startup before acknowledging that
	// the local survivor is presentation-ready.
	RestoreGameplayInputAfterMigration();

	PendingRecoveryReadyMatchId = MatchId;
	PendingRecoveryReadyEpoch = AuthorityEpoch;
	PendingRecoveryReadyRosterRevision = RosterRevision;
	PendingRecoveryReadyAttemptId = RecoveryAttemptId;
	bRecoveryReadyAcknowledgementPending = true;
	GetWorldTimerManager().SetTimerForNextTick(
		this,
		&AWeaponPlayerController::SendPendingRecoveryReadyAcknowledgement);
}

void AWeaponPlayerController::SendPendingRecoveryReadyAcknowledgement()
{
	if (!bRecoveryReadyAcknowledgementPending)
	{
		return;
	}
	bRecoveryReadyAcknowledgementPending = false;
	ServerAcknowledgeRecoveryReady(
		PendingRecoveryReadyMatchId,
		PendingRecoveryReadyEpoch,
		PendingRecoveryReadyRosterRevision,
		PendingRecoveryReadyAttemptId);
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

void AWeaponPlayerController::ServerConfirmMigrationCommonCommitted_Implementation(
	const FWeaponMigrationPayloadAck& Ack)
{
	UWeaponHostMigrationSubsystem* MigrationSubsystem =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>() : nullptr;
	FString Error;
	if (!MigrationSubsystem ||
		!MigrationSubsystem->RecordCommonCommittedConfirmation(this, Ack, Error))
	{
		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("CommonCommitted confirmation rejected for revision %lld: %s"),
			Ack.SnapshotRevision,
			MigrationSubsystem ? *Error : TEXT("HostMigrationSubsystem is unavailable"));
	}
}

void AWeaponPlayerController::ServerAcknowledgeRecoveryReady_Implementation(
	const FGuid MatchId,
	const uint64 AuthorityEpoch,
	const uint64 RosterRevision,
	const FGuid RecoveryAttemptId)
{
	UWeaponHostMigrationSubsystem* MigrationSubsystem =
		GetGameInstance() ? GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>() : nullptr;
	FString Error;
	if (!MigrationSubsystem || !MigrationSubsystem->RecordRecoveryReadyAcknowledgement(
		this,
		MatchId,
		AuthorityEpoch,
		RosterRevision,
		RecoveryAttemptId,
		Error))
	{
		UE_LOG(
			LogLetsDeepRock,
			Warning,
			TEXT("RecoveryReady ACK rejected for Match %s Epoch %llu Roster %llu Attempt %s: %s"),
			*MatchId.ToString(),
			AuthorityEpoch,
			RosterRevision,
			*RecoveryAttemptId.ToString(),
			MigrationSubsystem ? *Error : TEXT("HostMigrationSubsystem is unavailable"));
	}
}
