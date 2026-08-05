// Copyright Epic Games, Inc. All Rights Reserved.

#include "Online/Tests/WeaponHostMigrationHarnessSubsystem.h"

#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "GameFramework/PlayerState.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "Online/WeaponHostMigrationSubsystem.h"
#include "WeaponPlayerState.h"

DEFINE_LOG_CATEGORY_STATIC(LogWeaponMigrationHarness, Log, All);

namespace WeaponMigrationHarness
{
	constexpr float TickIntervalSeconds = 0.25f;
	constexpr double FindRetrySeconds = 1.0;
	constexpr int32 MaxFindResults = 50;
}

bool UWeaponHostMigrationHarnessSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
#if UE_BUILD_SHIPPING
	return false;
#else
	FString RequestedRole;
	return Super::ShouldCreateSubsystem(Outer) &&
		FParse::Value(
			FCommandLine::Get(),
			TEXT("WeaponMigrationHarnessRole="),
			RequestedRole) &&
		!RequestedRole.IsEmpty();
#endif
}

void UWeaponHostMigrationHarnessSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UWeaponSessionSubsystem>();
	Collection.InitializeDependency<UWeaponHostMigrationSubsystem>();

	FString RequestedRole;
	FParse::Value(
		FCommandLine::Get(),
		TEXT("WeaponMigrationHarnessRole="),
		RequestedRole);
	ProcessLabel = SanitizeMarkerValue(RequestedRole);
	if (RequestedRole.Equals(TEXT("Host"), ESearchCase::IgnoreCase))
	{
		ProcessRole = EProcessRole::Host;
	}
	else if (RequestedRole.Equals(TEXT("Client1"), ESearchCase::IgnoreCase) ||
		RequestedRole.Equals(TEXT("Client2"), ESearchCase::IgnoreCase) ||
		RequestedRole.Equals(TEXT("Client3"), ESearchCase::IgnoreCase))
	{
		ProcessRole = EProcessRole::Client;
	}
	else
	{
		UE_LOG(
			LogWeaponMigrationHarness,
			Error,
			TEXT("Invalid -WeaponMigrationHarnessRole=%s"),
			*RequestedRole);
		return;
	}

	if (!FParse::Value(FCommandLine::Get(), TEXT("WeaponMigrationHarnessRunId="), RunId))
	{
		RunId = TEXT("Manual");
	}
	if (!FParse::Value(FCommandLine::Get(), TEXT("WeaponMigrationHarnessServer="), ServerName))
	{
		ServerName = TEXT("WeaponMigrationHarness");
	}
	RunId = SanitizeMarkerValue(MoveTemp(RunId));
	ServerName = SanitizeMarkerValue(MoveTemp(ServerName));

	UWeaponSessionSubsystem* SessionSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	if (!SessionSubsystem)
	{
		EmitMarker(TEXT("DriverFailed"), TEXT("Reason=SessionSubsystemUnavailable"));
		return;
	}
	SessionSubsystem->OnHostComplete.AddDynamic(
		this,
		&UWeaponHostMigrationHarnessSubsystem::HandleHostComplete);
	SessionSubsystem->OnFindComplete.AddDynamic(
		this,
		&UWeaponHostMigrationHarnessSubsystem::HandleFindComplete);
	SessionSubsystem->OnJoinComplete.AddDynamic(
		this,
		&UWeaponHostMigrationHarnessSubsystem::HandleJoinComplete);

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(
			this,
			&UWeaponHostMigrationHarnessSubsystem::TickHarness),
		WeaponMigrationHarness::TickIntervalSeconds);
	EmitMarker(TEXT("DriverReady"), FString::Printf(TEXT("Server=%s"), *ServerName));
}

void UWeaponHostMigrationHarnessSubsystem::Deinitialize()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	if (UWeaponSessionSubsystem* SessionSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>())
	{
		SessionSubsystem->OnHostComplete.RemoveDynamic(
			this,
			&UWeaponHostMigrationHarnessSubsystem::HandleHostComplete);
		SessionSubsystem->OnFindComplete.RemoveDynamic(
			this,
			&UWeaponHostMigrationHarnessSubsystem::HandleFindComplete);
		SessionSubsystem->OnJoinComplete.RemoveDynamic(
			this,
			&UWeaponHostMigrationHarnessSubsystem::HandleJoinComplete);
	}
	Super::Deinitialize();
}

bool UWeaponHostMigrationHarnessSubsystem::TickHarness(const float DeltaSeconds)
{
	(void)DeltaSeconds;
	DriveInitialSession();
	ObserveMigration();
	return true;
}

void UWeaponHostMigrationHarnessSubsystem::DriveInitialSession()
{
	UWorld* World = GetWorld();
	UWeaponSessionSubsystem* SessionSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	if (!World || !World->IsGameWorld() || !SessionSubsystem ||
		SessionSubsystem->GetCurrentOperation() != EWeaponSessionOperation::Idle)
	{
		return;
	}

	if (ProcessRole == EProcessRole::Host)
	{
		if (!bHostRequestStarted && World->GetNetMode() == NM_Standalone)
		{
			bHostRequestStarted = SessionSubsystem->HostLanGame(4, ServerName);
			EmitMarker(
				bHostRequestStarted ? TEXT("HostRequested") : TEXT("HostRequestRejected"),
				FString::Printf(TEXT("Map=%s"), *World->GetOutermost()->GetName()));
		}
		return;
	}

	if (ProcessRole != EProcessRole::Client || bInitialJoinStarted || bFindInFlight ||
		World->GetNetMode() != NM_Standalone ||
		FPlatformTime::Seconds() < NextFindAttemptSeconds)
	{
		return;
	}

	bFindInFlight = SessionSubsystem->FindLanGames(WeaponMigrationHarness::MaxFindResults);
	NextFindAttemptSeconds = FPlatformTime::Seconds() + WeaponMigrationHarness::FindRetrySeconds;
	EmitMarker(
		bFindInFlight ? TEXT("FindRequested") : TEXT("FindRequestRejected"));
}

void UWeaponHostMigrationHarnessSubsystem::ObserveMigration()
{
	UWorld* World = GetWorld();
	UWeaponHostMigrationSubsystem* MigrationSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>();
	if (!World || !World->IsGameWorld() || !MigrationSubsystem)
	{
		return;
	}

	APlayerController* LocalController =
		GetGameInstance()->GetFirstLocalPlayerController(World);
	const AWeaponPlayerState* LocalPlayerState = LocalController
		? LocalController->GetPlayerState<AWeaponPlayerState>()
		: nullptr;
	const FUniqueNetIdRepl LocalPlayerId = LocalPlayerState
		? LocalPlayerState->GetUniqueId()
		: FUniqueNetIdRepl();
	const EWeaponMigrationRole LocalRole = LocalPlayerId.IsValid()
		? MigrationSubsystem->GetRoleForPlayer(LocalPlayerId)
		: EWeaponMigrationRole::None;

	if (ProcessRole == EProcessRole::Client && bInitialJoinStarted &&
		!bInitialConnectionObserved && World->GetNetMode() == NM_Client &&
		LocalPlayerId.IsValid())
	{
		bInitialConnectionObserved = true;
		EmitMarker(
			TEXT("InitialConnectionObserved"),
			FString::Printf(
				TEXT("Player=%s|NetMode=%s"),
				*SanitizeMarkerValue(LocalPlayerId.ToDebugString()),
				LexNetMode(World->GetNetMode())));
	}

	const EWeaponHostMigrationState MigrationState =
		MigrationSubsystem->GetMigrationState();
	const FWeaponMigrationRoleAssignment* Assignment =
		MigrationSubsystem->GetCurrentRoleAssignment();
	if (MigrationState != LastMigrationState)
	{
		if (LastMigrationState == EWeaponHostMigrationState::Monitoring &&
			MigrationState != EWeaponHostMigrationState::Monitoring)
		{
			bSawRecoveryTransition = true;
		}
		EmitMarker(
			TEXT("MigrationState"),
			Assignment
				? FString::Printf(
					TEXT("State=%s|Role=%s|NetMode=%s|Match=%s|Epoch=%llu|Roster=%llu"),
					LexMigrationState(MigrationState),
					LexMigrationRole(LocalRole),
					LexNetMode(World->GetNetMode()),
					*Assignment->MatchId.ToString(EGuidFormats::DigitsWithHyphens),
					Assignment->AuthorityEpoch,
					Assignment->RosterRevision)
				: FString::Printf(
					TEXT("State=%s|Role=%s|NetMode=%s"),
					LexMigrationState(MigrationState),
					LexMigrationRole(LocalRole),
					LexNetMode(World->GetNetMode())));
		LastMigrationState = MigrationState;
	}

	FWeaponMigrationPayloadAck CommonAck;
	const bool bHasSharedBaseline = Assignment &&
		Assignment->BackupA.IsAssigned() &&
		Assignment->BackupB.IsAssigned() &&
		MigrationSubsystem->GetCommonCommittedAck(CommonAck) &&
		CommonAck.MatchId == Assignment->MatchId &&
		CommonAck.AuthorityEpoch == Assignment->AuthorityEpoch &&
		CommonAck.RosterRevision == Assignment->RosterRevision &&
		LocalPlayerId.IsValid();
	if (!bSawRecoveryTransition && bHasSharedBaseline &&
		LocalRole != EWeaponMigrationRole::None &&
		(!bBaselineEmitted || !LastBaselineMarkerAck.IdentifiesSamePayload(CommonAck)))
	{
		bBaselineEmitted = true;
		LastBaselineMarkerAck = CommonAck;
		EmitMarker(
			TEXT("BaselineReady"),
			FString::Printf(
				TEXT("Player=%s|Role=%s|Slot=%d|Match=%s|Epoch=%llu|Roster=%llu|Snapshot=%lld|Hash=%s"),
				*SanitizeMarkerValue(LocalPlayerId.ToDebugString()),
				LexMigrationRole(LocalRole),
				LocalPlayerState ? LocalPlayerState->GetPlayerSlotIndex() : INDEX_NONE,
				*CommonAck.MatchId.ToString(EGuidFormats::DigitsWithHyphens),
				CommonAck.AuthorityEpoch,
				CommonAck.RosterRevision,
				CommonAck.SnapshotRevision,
				*BytesToHex(CommonAck.PayloadHash.GetData(), CommonAck.PayloadHash.Num())));
	}
	else if (!bObserverReadyEmitted && bHasSharedBaseline &&
		ProcessLabel == TEXT("Client3") && LocalRole == EWeaponMigrationRole::None)
	{
		bObserverReadyEmitted = true;
		EmitMarker(
			TEXT("ObserverReady"),
			FString::Printf(
				TEXT("Player=%s|Role=%s|Match=%s|Epoch=%llu|Roster=%llu|Snapshot=%lld|Hash=%s|NetMode=%s"),
				*SanitizeMarkerValue(LocalPlayerId.ToDebugString()),
				LexMigrationRole(LocalRole),
				*CommonAck.MatchId.ToString(EGuidFormats::DigitsWithHyphens),
				CommonAck.AuthorityEpoch,
				CommonAck.RosterRevision,
				CommonAck.SnapshotRevision,
				*BytesToHex(CommonAck.PayloadHash.GetData(), CommonAck.PayloadHash.Num()),
				LexNetMode(World->GetNetMode())));
	}

	if (!bRecoveryLiveEmitted && bBaselineEmitted && bSawRecoveryTransition &&
		MigrationState == EWeaponHostMigrationState::Monitoring && Assignment &&
		LocalPlayerId.IsValid() && LocalRole != EWeaponMigrationRole::None &&
		MigrationSubsystem->GetCommonCommittedAck(CommonAck))
	{
		bRecoveryLiveEmitted = true;
		EmitMarker(
			TEXT("RecoveryLive"),
			FString::Printf(
				TEXT("Player=%s|Role=%s|Match=%s|Epoch=%llu|Roster=%llu|Snapshot=%lld|Hash=%s|NetMode=%s"),
				*SanitizeMarkerValue(LocalPlayerId.ToDebugString()),
				LexMigrationRole(LocalRole),
				*CommonAck.MatchId.ToString(EGuidFormats::DigitsWithHyphens),
				Assignment->AuthorityEpoch,
				Assignment->RosterRevision,
				CommonAck.SnapshotRevision,
				*BytesToHex(CommonAck.PayloadHash.GetData(), CommonAck.PayloadHash.Num()),
				LexNetMode(World->GetNetMode())));
	}

	if (!bRecoveryLobbyEmitted && bObserverReadyEmitted && bSawRecoveryTransition &&
		MigrationState == EWeaponHostMigrationState::Failed &&
		World->GetNetMode() == NM_Standalone &&
		MigrationSubsystem->GetCommonCommittedAck(CommonAck))
	{
		bRecoveryLobbyEmitted = true;
		EmitMarker(
			TEXT("RecoveryLobby"),
			FString::Printf(
				TEXT("Player=%s|Role=%s|Match=%s|BaselineEpoch=%llu|BaselineRoster=%llu|Snapshot=%lld|Hash=%s|NetMode=%s"),
				*SanitizeMarkerValue(LocalPlayerId.ToDebugString()),
				LexMigrationRole(LocalRole),
				*CommonAck.MatchId.ToString(EGuidFormats::DigitsWithHyphens),
				CommonAck.AuthorityEpoch,
				CommonAck.RosterRevision,
				CommonAck.SnapshotRevision,
				*BytesToHex(CommonAck.PayloadHash.GetData(), CommonAck.PayloadHash.Num()),
				LexNetMode(World->GetNetMode())));
	}
}

void UWeaponHostMigrationHarnessSubsystem::HandleHostComplete(
	const bool bWasSuccessful,
	const FString Message)
{
	EmitMarker(
		bWasSuccessful ? TEXT("HostAdvertised") : TEXT("HostFailed"),
		FString::Printf(TEXT("Message=%s"), *SanitizeMarkerValue(Message)));
}

void UWeaponHostMigrationHarnessSubsystem::HandleFindComplete(
	const bool bWasSuccessful,
	const TArray<FWeaponLanSessionResult>& Results)
{
	bFindInFlight = false;
	if (ProcessRole != EProcessRole::Client || bInitialJoinStarted)
	{
		return;
	}

	const FWeaponLanSessionResult* MatchingResult = Results.FindByPredicate(
		[this](const FWeaponLanSessionResult& Result)
		{
			return Result.ServerName == ServerName;
		});
	if (!bWasSuccessful || !MatchingResult)
	{
		NextFindAttemptSeconds = FPlatformTime::Seconds() +
			WeaponMigrationHarness::FindRetrySeconds;
		EmitMarker(
			TEXT("FindCompleted"),
			FString::Printf(
				TEXT("Success=%d|Count=%d|Match=0"),
				bWasSuccessful,
				Results.Num()));
		return;
	}

	UWeaponSessionSubsystem* SessionSubsystem =
		GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	bInitialJoinStarted = SessionSubsystem &&
		SessionSubsystem->JoinLanGame(MatchingResult->ResultIndex);
	EmitMarker(
		bInitialJoinStarted ? TEXT("JoinRequested") : TEXT("JoinRequestRejected"),
		FString::Printf(
			TEXT("Result=%d|Ping=%d"),
			MatchingResult->ResultIndex,
			MatchingResult->PingInMs));
	if (!bInitialJoinStarted)
	{
		NextFindAttemptSeconds = FPlatformTime::Seconds() +
			WeaponMigrationHarness::FindRetrySeconds;
	}
}

void UWeaponHostMigrationHarnessSubsystem::HandleJoinComplete(
	const bool bWasSuccessful,
	const FString Message)
{
	EmitMarker(
		bWasSuccessful ? TEXT("JoinTravelStarted") : TEXT("JoinFailed"),
		FString::Printf(TEXT("Message=%s"), *SanitizeMarkerValue(Message)));
	if (!bWasSuccessful)
	{
		bInitialJoinStarted = false;
		NextFindAttemptSeconds = FPlatformTime::Seconds() +
			WeaponMigrationHarness::FindRetrySeconds;
	}
}

void UWeaponHostMigrationHarnessSubsystem::EmitMarker(
	const TCHAR* Event,
	const FString& Details) const
{
	if (Details.IsEmpty())
	{
		UE_LOG(
			LogWeaponMigrationHarness,
			Display,
			TEXT("WMH|Run=%s|Process=%s|Event=%s"),
			*RunId,
			*ProcessLabel,
			Event);
		return;
	}
	UE_LOG(
		LogWeaponMigrationHarness,
		Display,
		TEXT("WMH|Run=%s|Process=%s|Event=%s|%s"),
		*RunId,
		*ProcessLabel,
		Event,
		*Details);
}

const TCHAR* UWeaponHostMigrationHarnessSubsystem::LexMigrationState(
	const EWeaponHostMigrationState State)
{
	switch (State)
	{
	case EWeaponHostMigrationState::Idle: return TEXT("Idle");
	case EWeaponHostMigrationState::Monitoring: return TEXT("Monitoring");
	case EWeaponHostMigrationState::WaitingForDisconnectTravel: return TEXT("WaitingForDisconnectTravel");
	case EWeaponHostMigrationState::PreparingCandidate: return TEXT("PreparingCandidate");
	case EWeaponHostMigrationState::SearchingCandidate: return TEXT("SearchingCandidate");
	case EWeaponHostMigrationState::JoiningCandidate: return TEXT("JoiningCandidate");
	case EWeaponHostMigrationState::Restoring: return TEXT("Restoring");
	case EWeaponHostMigrationState::AdvertisingCandidate: return TEXT("AdvertisingCandidate");
	case EWeaponHostMigrationState::WaitingForReadyAcks: return TEXT("WaitingForReadyAcks");
	case EWeaponHostMigrationState::PlannedHandoff: return TEXT("PlannedHandoff");
	case EWeaponHostMigrationState::LoadingLocalFork: return TEXT("LoadingLocalFork");
	case EWeaponHostMigrationState::PreparingLocalFork: return TEXT("PreparingLocalFork");
	case EWeaponHostMigrationState::RestoringLocalFork: return TEXT("RestoringLocalFork");
	case EWeaponHostMigrationState::AdvertisingLocalFork: return TEXT("AdvertisingLocalFork");
	case EWeaponHostMigrationState::Failed: return TEXT("Failed");
	default: return TEXT("Unknown");
	}
}

const TCHAR* UWeaponHostMigrationHarnessSubsystem::LexMigrationRole(
	const EWeaponMigrationRole Role)
{
	switch (Role)
	{
	case EWeaponMigrationRole::None: return TEXT("None");
	case EWeaponMigrationRole::ActiveHost: return TEXT("ActiveHost");
	case EWeaponMigrationRole::BackupA: return TEXT("BackupA");
	case EWeaponMigrationRole::BackupB: return TEXT("BackupB");
	default: return TEXT("Unknown");
	}
}

const TCHAR* UWeaponHostMigrationHarnessSubsystem::LexNetMode(const ENetMode NetMode)
{
	switch (NetMode)
	{
	case NM_Standalone: return TEXT("Standalone");
	case NM_DedicatedServer: return TEXT("DedicatedServer");
	case NM_ListenServer: return TEXT("ListenServer");
	case NM_Client: return TEXT("Client");
	default: return TEXT("Unknown");
	}
}

FString UWeaponHostMigrationHarnessSubsystem::SanitizeMarkerValue(FString Value)
{
	Value.ReplaceInline(TEXT("|"), TEXT("/"));
	Value.ReplaceInline(TEXT("\r"), TEXT(" "));
	Value.ReplaceInline(TEXT("\n"), TEXT(" "));
	return Value;
}
