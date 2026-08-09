// Copyright Epic Games, Inc. All Rights Reserved.

#include "Online/WeaponSessionSubsystem.h"

#include "Engine/EngineBaseTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "Misc/NetworkVersion.h"
#include "OnlineSubsystemUtils.h"
#include "UObject/UObjectGlobals.h"
#include "WeaponGameMode.h"
#include "WeaponGameState.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogWeaponSession, Log, All);

namespace WeaponSession
{
	const FName GameSessionName(TEXT("GameSession"));
	const FName MapNameSetting(TEXT("MAPNAME"));
	const FName ServerNameSetting(TEXT("WEAPON_SERVER_NAME"));
	const FName RecoverySetting(TEXT("WEAPON_RECOVERY"));
	const FName RecoveryProtocolSetting(TEXT("WEAPON_RECOVERY_PROTOCOL"));
	const FName RecoveryMatchIdSetting(TEXT("WEAPON_RECOVERY_MATCH"));
	const FName RecoveryEpochSetting(TEXT("WEAPON_RECOVERY_EPOCH"));
	const FName RecoverySnapshotRevisionSetting(TEXT("WEAPON_RECOVERY_REVISION"));
	const FName RecoveryRosterRevisionSetting(TEXT("WEAPON_RECOVERY_ROSTER"));
	const FName RecoveryAttemptIdSetting(TEXT("WEAPON_RECOVERY_ATTEMPT"));
	const FName RecoveryCandidateIdSetting(TEXT("WEAPON_RECOVERY_CANDIDATE"));
	const FName RecoveryCandidateSlotSetting(TEXT("WEAPON_RECOVERY_SLOT"));
	const FName RecoveryPayloadHashSetting(TEXT("WEAPON_RECOVERY_HASH"));
	const FName RecoveryStateSetting(TEXT("WEAPON_RECOVERY_STATE"));
	const FName RecoveryBuildSetting(TEXT("WEAPON_RECOVERY_BUILD"));
}

void UWeaponSessionSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	UE_LOG(LogWeaponSession, Log, TEXT("Weapon Session Subsystem initialized."));
}

void UWeaponSessionSubsystem::Deinitialize()
{
	ClearOnlineDelegates();

	if (PostLoadMapDelegateHandle.IsValid())
	{
		FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapDelegateHandle);
		PostLoadMapDelegateHandle.Reset();
	}

	ActiveSearch.Reset();
	CachedLanResults.Reset();
	PendingAction = EPendingAction::None;
	ResetRecoveryOperationFlags();
	SetOperation(EWeaponSessionOperation::Idle);
	Super::Deinitialize();
}

IOnlineSessionPtr UWeaponSessionSubsystem::GetSessionInterface() const
{
	return Online::GetSessionInterface(GetWorld());
}

FName UWeaponSessionSubsystem::ResolveCurrentMapName() const
{
	const UWorld* World = GetWorld();
	if (!World)
	{
		return NAME_None;
	}

	return FName(*UWorld::RemovePIEPrefix(World->GetOutermost()->GetName()));
}

bool UWeaponSessionSubsystem::HostLanGame(
	const int32 MaxPlayers,
	const FString& ServerName,
	const FName TargetMapName)
{
	if (CurrentOperation != EWeaponSessionOperation::Idle || MaxPlayers < 1 || MaxPlayers > 4)
	{
		return false;
	}

	PendingHostMapName = TargetMapName.IsNone() ? ResolveCurrentMapName() : TargetMapName;
	if (PendingHostMapName.IsNone())
	{
		BroadcastHostComplete(false, TEXT("Unable to resolve the current map."));
		return false;
	}

	PendingHostMaxPlayers = MaxPlayers;
	PendingHostServerName = ServerName.TrimStartAndEnd();
	if (PendingHostServerName.IsEmpty())
	{
		PendingHostServerName = TEXT("LetsDeepRock LAN");
	}

	PendingAction = EPendingAction::Host;

	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid() && Sessions->GetNamedSession(WeaponSession::GameSessionName))
	{
		return BeginDestroySession();
	}

	return BeginHostTravel();
}

bool UWeaponSessionSubsystem::PrepareRecoveryHost(
	const FWeaponRecoverySessionDescriptor& Descriptor)
{
	if (CurrentOperation != EWeaponSessionOperation::Idle || !Descriptor.IsValid(true) ||
		Descriptor.State != EWeaponRecoverySessionState::Restoring)
	{
		return false;
	}

	PendingRecoverySession = Descriptor;
	PendingAction = EPendingAction::RecoveryHost;
	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid() && Sessions->GetNamedSession(WeaponSession::GameSessionName))
	{
		return BeginDestroySession();
	}
	return BeginRecoveryHostTravel();
}

bool UWeaponSessionSubsystem::BeginHostTravel()
{
	UWorld* World = GetWorld();
	if (!World || PendingHostMapName.IsNone())
	{
		FailPendingAction(TEXT("A valid world and host map are required."));
		return false;
	}

	if (World->GetNetMode() == NM_ListenServer)
	{
		return BeginCreateSession();
	}

	SetOperation(EWeaponSessionOperation::HostingTravel);

	if (!PostLoadMapDelegateHandle.IsValid())
	{
		PostLoadMapDelegateHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
			this,
			&UWeaponSessionSubsystem::HandlePostLoadMap);
	}

	UGameplayStatics::OpenLevel(this, PendingHostMapName, true, TEXT("listen"));
	return true;
}

bool UWeaponSessionSubsystem::BeginRecoveryHostTravel()
{
	UWorld* World = GetWorld();
	if (!World || !PendingRecoverySession.IsValid(true))
	{
		FailPendingAction(TEXT("A valid recovery descriptor and World are required."));
		return false;
	}

	SetOperation(EWeaponSessionOperation::HostingTravel);
	if (!PostLoadMapDelegateHandle.IsValid())
	{
		PostLoadMapDelegateHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
			this,
			&UWeaponSessionSubsystem::HandlePostLoadMap);
	}

	UGameplayStatics::OpenLevel(
		this,
		FName(*PendingRecoverySession.MapPackageName),
		true,
		TEXT("listen?WeaponRecovery=1"));
	return true;
}

void UWeaponSessionSubsystem::HandlePostLoadMap(UWorld* LoadedWorld)
{
	if (!LoadedWorld || LoadedWorld->GetGameInstance() != GetGameInstance() ||
		(PendingAction != EPendingAction::Host &&
		 PendingAction != EPendingAction::RecoveryHost &&
		 PendingAction != EPendingAction::RecoveryLobby))
	{
		return;
	}
	const EPendingAction CompletedTravel = PendingAction;

	if (PostLoadMapDelegateHandle.IsValid())
	{
		FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapDelegateHandle);
		PostLoadMapDelegateHandle.Reset();
	}

	if (CompletedTravel == EPendingAction::Host)
	{
		BeginCreateSession();
		return;
	}

	PendingAction = EPendingAction::None;
	SetOperation(EWeaponSessionOperation::Idle);
	if (CompletedTravel == EPendingAction::RecoveryLobby)
	{
		return;
	}

	AWeaponGameMode* GameMode = LoadedWorld->GetAuthGameMode<AWeaponGameMode>();
	const bool bPrepared = LoadedWorld->GetNetMode() == NM_ListenServer &&
		GameMode && GameMode->EnterGameplayRecoveryBarrier();
	OnRecoveryWorldPrepared.Broadcast(
		bPrepared,
		bPrepared
			? TEXT("Recovery listen World is gated and ready for Restore.")
			: TEXT("Recovery listen World could not enter the gameplay barrier."));
}

bool UWeaponSessionSubsystem::BeginCreateSession()
{
	return BeginCreateSessionInternal(nullptr);
}

bool UWeaponSessionSubsystem::BeginCreateSessionInternal(
	const FWeaponRecoverySessionDescriptor* RecoveryDescriptor)
{
	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (!Sessions.IsValid())
	{
		FailPendingAction(TEXT("Online Session interface is unavailable."));
		return false;
	}

	const int32 HostMaxPlayers = RecoveryDescriptor
		? RecoveryDescriptor->MaxPlayers
		: PendingHostMaxPlayers;
	const FName HostMapName = RecoveryDescriptor
		? FName(*RecoveryDescriptor->MapPackageName)
		: PendingHostMapName;
	const FString HostServerName = RecoveryDescriptor
		? TEXT("LetsDeepRock Recovery")
		: PendingHostServerName;
	AWeaponGameState* GameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	if (!GameState || !GameState->SetMaxPlayersAuthority(HostMaxPlayers))
	{
		FailPendingAction(TEXT("Failed to apply host capacity to AWeaponGameState."));
		return false;
	}

	PendingAction = EPendingAction::None;
	SetOperation(EWeaponSessionOperation::Creating);

	FOnlineSessionSettings Settings;
	Settings.bIsLANMatch = true;
	Settings.bShouldAdvertise = true;
	Settings.bAllowJoinInProgress = true;
	Settings.bAllowInvites = true;
	Settings.bUsesPresence = false;
	Settings.bAllowJoinViaPresence = false;
	Settings.bIsDedicated = false;
	Settings.bAntiCheatProtected = false;
	Settings.NumPublicConnections = HostMaxPlayers;
	Settings.NumPrivateConnections = 0;
	Settings.Set(WeaponSession::MapNameSetting, HostMapName.ToString(), EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	Settings.Set(WeaponSession::ServerNameSetting, HostServerName, EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	if (RecoveryDescriptor)
	{
		Settings.Set(WeaponSession::RecoverySetting, true, EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
		Settings.Set(WeaponSession::RecoveryProtocolSetting, WeaponMigrationProtocolVersion, EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
		Settings.Set(WeaponSession::RecoveryMatchIdSetting, RecoveryDescriptor->MatchId.ToString(EGuidFormats::DigitsWithHyphens), EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
		Settings.Set(WeaponSession::RecoveryEpochSetting, LexToString(RecoveryDescriptor->AuthorityEpoch), EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
		Settings.Set(WeaponSession::RecoverySnapshotRevisionSetting, LexToString(RecoveryDescriptor->BaseSnapshotRevision), EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
		Settings.Set(WeaponSession::RecoveryRosterRevisionSetting, LexToString(RecoveryDescriptor->BaseRosterRevision), EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
		Settings.Set(WeaponSession::RecoveryAttemptIdSetting, RecoveryDescriptor->RecoveryAttemptId.ToString(EGuidFormats::DigitsWithHyphens), EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
		Settings.Set(WeaponSession::RecoveryCandidateIdSetting, RecoveryDescriptor->Candidate.PlayerId.ToDebugString(), EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
		Settings.Set(WeaponSession::RecoveryCandidateSlotSetting, RecoveryDescriptor->Candidate.OriginalSlot, EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
		Settings.Set(WeaponSession::RecoveryPayloadHashSetting, BytesToHex(RecoveryDescriptor->PayloadHash.GetData(), RecoveryDescriptor->PayloadHash.Num()), EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
		Settings.Set(WeaponSession::RecoveryStateSetting, static_cast<int32>(RecoveryDescriptor->State), EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
		Settings.Set(WeaponSession::RecoveryBuildSetting, LexToString(FNetworkVersion::GetLocalNetworkVersion()), EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	}
	bCreatingRecoverySession = RecoveryDescriptor != nullptr;

	CreateSessionDelegateHandle = Sessions->AddOnCreateSessionCompleteDelegate_Handle(
		FOnCreateSessionCompleteDelegate::CreateUObject(
			this,
			&UWeaponSessionSubsystem::HandleCreateSessionComplete));

	const bool bRequestStarted = Sessions->CreateSession(0, WeaponSession::GameSessionName, Settings);
	if (!bRequestStarted && CurrentOperation == EWeaponSessionOperation::Creating)
	{
		Sessions->ClearOnCreateSessionCompleteDelegate_Handle(CreateSessionDelegateHandle);
		CreateSessionDelegateHandle.Reset();
		SetOperation(EWeaponSessionOperation::Idle);
		if (bCreatingRecoverySession)
		{
			bCreatingRecoverySession = false;
			OnRecoveryHostComplete.Broadcast(false, TEXT("Recovery CreateSession request was rejected."));
		}
		else
		{
			BroadcastHostComplete(false, TEXT("CreateSession request was rejected."));
		}
	}

	return bRequestStarted;
}

void UWeaponSessionSubsystem::HandleCreateSessionComplete(const FName SessionName, const bool bWasSuccessful)
{
	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid() && CreateSessionDelegateHandle.IsValid())
	{
		Sessions->ClearOnCreateSessionCompleteDelegate_Handle(CreateSessionDelegateHandle);
		CreateSessionDelegateHandle.Reset();
	}

	SetOperation(EWeaponSessionOperation::Idle);
	const bool bSuccess = bWasSuccessful && SessionName == WeaponSession::GameSessionName;
	if (bCreatingRecoverySession)
	{
		bCreatingRecoverySession = false;
		OnRecoveryHostComplete.Broadcast(
			bSuccess,
			bSuccess
				? TEXT("Ready Recovery Session is advertised.")
				: TEXT("Failed to create Recovery Session."));
	}
	else
	{
		BroadcastHostComplete(
			bSuccess,
			bSuccess ? TEXT("LAN host is listening and advertised.") : TEXT("Failed to create LAN Session."));
	}
}

bool UWeaponSessionSubsystem::AdvertiseRecoverySession(
	const FWeaponRecoverySessionDescriptor& Descriptor)
{
	const UWorld* World = GetWorld();
	const AWeaponGameState* GameState = World ? World->GetGameState<AWeaponGameState>() : nullptr;
	if (CurrentOperation != EWeaponSessionOperation::Idle || !Descriptor.IsValid(true) ||
		Descriptor.State != EWeaponRecoverySessionState::Ready ||
		!World || World->GetNetMode() != NM_ListenServer ||
		!GameState || !GameState->IsGameplayRecoveryBlocked())
	{
		return false;
	}

	PendingRecoverySession = Descriptor;
	return BeginCreateSessionInternal(&PendingRecoverySession);
}

bool UWeaponSessionSubsystem::FindLanGames(const int32 MaxResults)
{
	if (CurrentOperation != EWeaponSessionOperation::Idle || MaxResults < 1)
	{
		return false;
	}

	CachedLanResults.Reset();
	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (!Sessions.IsValid())
	{
		OnFindComplete.Broadcast(false, CachedLanResults);
		return false;
	}

	ActiveSearch = MakeShared<FOnlineSessionSearch>();
	ActiveSearch->bIsLanQuery = true;
	ActiveSearch->MaxSearchResults = MaxResults;
	bFindingRecoverySession = false;
	SetOperation(EWeaponSessionOperation::Finding);

	FindSessionsDelegateHandle = Sessions->AddOnFindSessionsCompleteDelegate_Handle(
		FOnFindSessionsCompleteDelegate::CreateUObject(
			this,
			&UWeaponSessionSubsystem::HandleFindSessionsComplete));

	const bool bRequestStarted = Sessions->FindSessions(0, ActiveSearch.ToSharedRef());
	if (!bRequestStarted && CurrentOperation == EWeaponSessionOperation::Finding)
	{
		Sessions->ClearOnFindSessionsCompleteDelegate_Handle(FindSessionsDelegateHandle);
		FindSessionsDelegateHandle.Reset();
		SetOperation(EWeaponSessionOperation::Idle);
		OnFindComplete.Broadcast(false, CachedLanResults);
	}

	return bRequestStarted;
}

bool UWeaponSessionSubsystem::FindAndJoinRecoverySession(
	const FWeaponRecoverySessionDescriptor& Expected)
{
	if (CurrentOperation != EWeaponSessionOperation::Idle || !Expected.IsValid(false) ||
		Expected.RecoveryAttemptId.IsValid() ||
		Expected.State != EWeaponRecoverySessionState::Ready)
	{
		return false;
	}

	ExpectedRecoverySession = Expected;
	bFindingRecoverySession = true;
	bHasSelectedRecoverySession = false;
	return BeginRecoverySearch();
}

bool UWeaponSessionSubsystem::BeginRecoverySearch()
{
	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (!Sessions.IsValid())
	{
		bFindingRecoverySession = false;
		OnRecoveryJoinComplete.Broadcast(false, TEXT("Online Session interface is unavailable."));
		return false;
	}

	ActiveSearch = MakeShared<FOnlineSessionSearch>();
	ActiveSearch->bIsLanQuery = true;
	ActiveSearch->MaxSearchResults = 50;
	SetOperation(EWeaponSessionOperation::Finding);
	FindSessionsDelegateHandle = Sessions->AddOnFindSessionsCompleteDelegate_Handle(
		FOnFindSessionsCompleteDelegate::CreateUObject(
			this,
			&UWeaponSessionSubsystem::HandleFindSessionsComplete));

	const bool bRequestStarted = Sessions->FindSessions(0, ActiveSearch.ToSharedRef());
	if (!bRequestStarted && CurrentOperation == EWeaponSessionOperation::Finding)
	{
		Sessions->ClearOnFindSessionsCompleteDelegate_Handle(FindSessionsDelegateHandle);
		FindSessionsDelegateHandle.Reset();
		SetOperation(EWeaponSessionOperation::Idle);
		bFindingRecoverySession = false;
		OnRecoveryJoinComplete.Broadcast(false, TEXT("Recovery FindSessions request was rejected."));
	}
	return bRequestStarted;
}

void UWeaponSessionSubsystem::HandleFindSessionsComplete(const bool bWasSuccessful)
{
	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid() && FindSessionsDelegateHandle.IsValid())
	{
		Sessions->ClearOnFindSessionsCompleteDelegate_Handle(FindSessionsDelegateHandle);
		FindSessionsDelegateHandle.Reset();
	}

	if (bFindingRecoverySession)
	{
		bFindingRecoverySession = false;
		SetOperation(EWeaponSessionOperation::Idle);
		if (bWasSuccessful && ActiveSearch.IsValid())
		{
			for (int32 Index = 0; Index < ActiveSearch->SearchResults.Num(); ++Index)
			{
				FWeaponRecoverySessionDescriptor FoundDescriptor;
				if (!TryReadMatchingRecoverySession(
					ActiveSearch->SearchResults[Index],
					FoundDescriptor))
				{
					continue;
				}

				SelectedRecoverySession = MoveTemp(FoundDescriptor);
				bHasSelectedRecoverySession = true;
				PendingJoinResultIndex = Index;
				PendingAction = EPendingAction::RecoveryJoinSearchResult;
				const IOnlineSessionPtr CurrentSessions = GetSessionInterface();
				if (CurrentSessions.IsValid() &&
					CurrentSessions->GetNamedSession(WeaponSession::GameSessionName))
				{
					BeginDestroySession();
				}
				else
				{
					bJoiningRecoverySession = true;
					BeginJoinSession(Index);
				}
				return;
			}
		}

		OnRecoveryJoinComplete.Broadcast(
			false,
			bWasSuccessful
				? TEXT("No matching Ready Recovery Session was found.")
				: TEXT("Recovery Session search failed."));
		return;
	}

	CachedLanResults.Reset();
	if (bWasSuccessful && ActiveSearch.IsValid())
	{
		for (int32 Index = 0; Index < ActiveSearch->SearchResults.Num(); ++Index)
		{
			const FOnlineSessionSearchResult& SearchResult = ActiveSearch->SearchResults[Index];
			FWeaponLanSessionResult& Result = CachedLanResults.AddDefaulted_GetRef();
			Result.ResultIndex = Index;
			Result.PingInMs = SearchResult.PingInMs;
			Result.MaxPlayers = SearchResult.Session.SessionSettings.NumPublicConnections;
			Result.CurrentPlayers = FMath::Max(0, Result.MaxPlayers - SearchResult.Session.NumOpenPublicConnections);
			Result.ServerName = SearchResult.Session.OwningUserName;
			SearchResult.Session.SessionSettings.Get(WeaponSession::ServerNameSetting, Result.ServerName);
			SearchResult.Session.SessionSettings.Get(WeaponSession::MapNameSetting, Result.MapName);
		}
	}

	SetOperation(EWeaponSessionOperation::Idle);
	OnFindComplete.Broadcast(bWasSuccessful, CachedLanResults);
}

bool UWeaponSessionSubsystem::TryReadMatchingRecoverySession(
	const FOnlineSessionSearchResult& SearchResult,
	FWeaponRecoverySessionDescriptor& OutDescriptor) const
{
	OutDescriptor = FWeaponRecoverySessionDescriptor();
	const FOnlineSessionSettings& Settings = SearchResult.Session.SessionSettings;
	bool bRecovery = false;
	int32 ProtocolVersion = 0;
	int32 CandidateSlot = INDEX_NONE;
	int32 RecoveryState = INDEX_NONE;
	FString MatchId;
	FString AuthorityEpoch;
	FString SnapshotRevision;
	FString RosterRevision;
	FString AttemptId;
	FString CandidateId;
	FString PayloadHash;
	FString MapName;
	FString BuildId;
	if (!Settings.Get(WeaponSession::RecoverySetting, bRecovery) || !bRecovery ||
		!Settings.Get(WeaponSession::RecoveryProtocolSetting, ProtocolVersion) ||
		ProtocolVersion != WeaponMigrationProtocolVersion ||
		!Settings.Get(WeaponSession::RecoveryMatchIdSetting, MatchId) ||
		!Settings.Get(WeaponSession::RecoveryEpochSetting, AuthorityEpoch) ||
		!Settings.Get(WeaponSession::RecoverySnapshotRevisionSetting, SnapshotRevision) ||
		!Settings.Get(WeaponSession::RecoveryRosterRevisionSetting, RosterRevision) ||
		!Settings.Get(WeaponSession::RecoveryAttemptIdSetting, AttemptId) ||
		!Settings.Get(WeaponSession::RecoveryCandidateIdSetting, CandidateId) ||
		!Settings.Get(WeaponSession::RecoveryCandidateSlotSetting, CandidateSlot) ||
		!Settings.Get(WeaponSession::RecoveryPayloadHashSetting, PayloadHash) ||
		!Settings.Get(WeaponSession::RecoveryStateSetting, RecoveryState) ||
		!Settings.Get(WeaponSession::RecoveryBuildSetting, BuildId) ||
		!Settings.Get(WeaponSession::MapNameSetting, MapName))
	{
		return false;
	}

	FGuid ParsedAttemptId;
	if (MatchId != ExpectedRecoverySession.MatchId.ToString(EGuidFormats::DigitsWithHyphens) ||
		AuthorityEpoch != LexToString(ExpectedRecoverySession.AuthorityEpoch) ||
		SnapshotRevision != LexToString(ExpectedRecoverySession.BaseSnapshotRevision) ||
		RosterRevision != LexToString(ExpectedRecoverySession.BaseRosterRevision) ||
		CandidateId != ExpectedRecoverySession.Candidate.PlayerId.ToDebugString() ||
		CandidateSlot != ExpectedRecoverySession.Candidate.OriginalSlot ||
		PayloadHash != BytesToHex(ExpectedRecoverySession.PayloadHash.GetData(), ExpectedRecoverySession.PayloadHash.Num()) ||
		RecoveryState != static_cast<int32>(EWeaponRecoverySessionState::Ready) ||
		BuildId != LexToString(FNetworkVersion::GetLocalNetworkVersion()) ||
		MapName != ExpectedRecoverySession.MapPackageName ||
		SearchResult.Session.SessionSettings.NumPublicConnections != ExpectedRecoverySession.MaxPlayers ||
		!FGuid::Parse(AttemptId, ParsedAttemptId) || !ParsedAttemptId.IsValid())
	{
		return false;
	}

	OutDescriptor = ExpectedRecoverySession;
	OutDescriptor.RecoveryAttemptId = ParsedAttemptId;
	return OutDescriptor.IsValid(true);
}

bool UWeaponSessionSubsystem::JoinLanGame(const int32 ResultIndex)
{
	if (CurrentOperation != EWeaponSessionOperation::Idle || !ActiveSearch.IsValid() ||
		!ActiveSearch->SearchResults.IsValidIndex(ResultIndex))
	{
		return false;
	}

	PendingJoinResultIndex = ResultIndex;
	PendingAction = EPendingAction::JoinSearchResult;
	bJoiningRecoverySession = false;

	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid() && Sessions->GetNamedSession(WeaponSession::GameSessionName))
	{
		return BeginDestroySession();
	}

	return BeginJoinSession(ResultIndex);
}

bool UWeaponSessionSubsystem::BeginJoinSession(const int32 ResultIndex)
{
	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (!Sessions.IsValid() || !ActiveSearch.IsValid() || !ActiveSearch->SearchResults.IsValidIndex(ResultIndex))
	{
		FailPendingAction(TEXT("The selected LAN Session is no longer available."));
		return false;
	}

	PendingAction = EPendingAction::None;
	SetOperation(EWeaponSessionOperation::Joining);
	JoinSessionDelegateHandle = Sessions->AddOnJoinSessionCompleteDelegate_Handle(
		FOnJoinSessionCompleteDelegate::CreateUObject(
			this,
			&UWeaponSessionSubsystem::HandleJoinSessionComplete));

	const bool bRequestStarted = Sessions->JoinSession(
		0,
		WeaponSession::GameSessionName,
		ActiveSearch->SearchResults[ResultIndex]);

	if (!bRequestStarted && CurrentOperation == EWeaponSessionOperation::Joining)
	{
		Sessions->ClearOnJoinSessionCompleteDelegate_Handle(JoinSessionDelegateHandle);
		JoinSessionDelegateHandle.Reset();
		SetOperation(EWeaponSessionOperation::Idle);
		if (bJoiningRecoverySession)
		{
			bJoiningRecoverySession = false;
			OnRecoveryJoinComplete.Broadcast(false, TEXT("Recovery JoinSession request was rejected."));
		}
		else
		{
			OnJoinComplete.Broadcast(false, TEXT("JoinSession request was rejected."));
		}
	}

	return bRequestStarted;
}

void UWeaponSessionSubsystem::HandleJoinSessionComplete(
	const FName SessionName,
	const EOnJoinSessionCompleteResult::Type Result)
{
	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid() && JoinSessionDelegateHandle.IsValid())
	{
		Sessions->ClearOnJoinSessionCompleteDelegate_Handle(JoinSessionDelegateHandle);
		JoinSessionDelegateHandle.Reset();
	}

	if (SessionName != WeaponSession::GameSessionName || Result != EOnJoinSessionCompleteResult::Success || !Sessions.IsValid())
	{
		SetOperation(EWeaponSessionOperation::Idle);
		if (bJoiningRecoverySession)
		{
			bJoiningRecoverySession = false;
			OnRecoveryJoinComplete.Broadcast(false, FString::Printf(TEXT("Recovery join failed: %s"), LexToString(Result)));
		}
		else
		{
			OnJoinComplete.Broadcast(false, FString::Printf(TEXT("Join failed: %s"), LexToString(Result)));
		}
		return;
	}

	FString ConnectString;
	if (!Sessions->GetResolvedConnectString(WeaponSession::GameSessionName, ConnectString))
	{
		SetOperation(EWeaponSessionOperation::Idle);
		if (bJoiningRecoverySession)
		{
			bJoiningRecoverySession = false;
			OnRecoveryJoinComplete.Broadcast(false, TEXT("The joined Recovery Session has no valid game address."));
		}
		else
		{
			OnJoinComplete.Broadcast(false, TEXT("The joined Session has no valid game address."));
		}
		return;
	}

	SetOperation(EWeaponSessionOperation::DirectTravel);
	if (APlayerController* PlayerController = GetGameInstance()->GetFirstLocalPlayerController(GetWorld()))
	{
		PlayerController->ClientTravel(ConnectString, TRAVEL_Absolute);
		SetOperation(EWeaponSessionOperation::Idle);
		if (bJoiningRecoverySession)
		{
			bJoiningRecoverySession = false;
			OnRecoveryJoinComplete.Broadcast(true, ConnectString);
		}
		else
		{
			OnJoinComplete.Broadcast(true, ConnectString);
		}
	}
	else
	{
		SetOperation(EWeaponSessionOperation::Idle);
		if (bJoiningRecoverySession)
		{
			bJoiningRecoverySession = false;
			OnRecoveryJoinComplete.Broadcast(false, TEXT("No local PlayerController is available for Recovery travel."));
		}
		else
		{
			OnJoinComplete.Broadcast(false, TEXT("No local PlayerController is available for travel."));
		}
	}
}

bool UWeaponSessionSubsystem::JoinDirectAddress(const FString& Address)
{
	if (CurrentOperation != EWeaponSessionOperation::Idle)
	{
		return false;
	}

	PendingDirectAddress = Address.TrimStartAndEnd();
	PendingAction = EPendingAction::DirectTravel;

	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid() && Sessions->GetNamedSession(WeaponSession::GameSessionName))
	{
		return BeginDestroySession();
	}

	return BeginDirectTravel(PendingDirectAddress);
}

bool UWeaponSessionSubsystem::ReconnectToMigratedHost(const FString& Address)
{
	return JoinDirectAddress(Address);
}

bool UWeaponSessionSubsystem::ReturnToRecoveryLobby(const FName MapPackageName)
{
	if (MapPackageName.IsNone())
	{
		return false;
	}

	ClearOnlineDelegates();
	ActiveSearch.Reset();
	ResetRecoveryOperationFlags();
	SetOperation(EWeaponSessionOperation::Idle);
	PendingRecoveryLobbyMapName = MapPackageName;
	PendingAction = EPendingAction::RecoveryLobby;

	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid() && Sessions->GetNamedSession(WeaponSession::GameSessionName))
	{
		return BeginDestroySession();
	}
	return BeginRecoveryLobbyTravel();
}

bool UWeaponSessionSubsystem::BeginDirectTravel(const FString& Address)
{
	FString NormalizedAddress = Address.TrimStartAndEnd();
	FURL ParsedUrl(nullptr, *NormalizedAddress, TRAVEL_Absolute);
	if (!ParsedUrl.Valid || ParsedUrl.Host.IsEmpty())
	{
		FailPendingAction(TEXT("Direct address is invalid. Use IPv4, [IPv6]:port, or hostname:port."));
		return false;
	}

	APlayerController* PlayerController = GetGameInstance()->GetFirstLocalPlayerController(GetWorld());
	if (!PlayerController)
	{
		FailPendingAction(TEXT("No local PlayerController is available for direct travel."));
		return false;
	}

	PendingAction = EPendingAction::None;
	SetOperation(EWeaponSessionOperation::DirectTravel);
	const FString TravelUrl = ParsedUrl.ToString();
	PlayerController->ClientTravel(TravelUrl, TRAVEL_Absolute);
	SetOperation(EWeaponSessionOperation::Idle);
	OnJoinComplete.Broadcast(true, TravelUrl);
	return true;
}

bool UWeaponSessionSubsystem::BeginRecoveryLobbyTravel()
{
	if (PendingRecoveryLobbyMapName.IsNone())
	{
		PendingAction = EPendingAction::None;
		return false;
	}

	SetOperation(EWeaponSessionOperation::DirectTravel);
	if (!PostLoadMapDelegateHandle.IsValid())
	{
		PostLoadMapDelegateHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
			this,
			&UWeaponSessionSubsystem::HandlePostLoadMap);
	}
	UGameplayStatics::OpenLevel(this, PendingRecoveryLobbyMapName, true);
	return true;
}

bool UWeaponSessionSubsystem::DestroySession()
{
	if (CurrentOperation != EWeaponSessionOperation::Idle)
	{
		return false;
	}

	PendingAction = EPendingAction::None;
	return BeginDestroySession();
}

bool UWeaponSessionSubsystem::BeginDestroySession()
{
	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (!Sessions.IsValid())
	{
		OnDestroyComplete.Broadcast(false, TEXT("Online Session interface is unavailable."));
		FailPendingAction(TEXT("Online Session interface is unavailable."));
		return false;
	}

	if (!Sessions->GetNamedSession(WeaponSession::GameSessionName))
	{
		OnDestroyComplete.Broadcast(true, TEXT("No local Session existed."));
		ExecutePendingAction();
		return true;
	}

	SetOperation(EWeaponSessionOperation::Destroying);
	DestroySessionDelegateHandle = Sessions->AddOnDestroySessionCompleteDelegate_Handle(
		FOnDestroySessionCompleteDelegate::CreateUObject(
			this,
			&UWeaponSessionSubsystem::HandleDestroySessionComplete));

	const bool bRequestStarted = Sessions->DestroySession(WeaponSession::GameSessionName);
	if (!bRequestStarted && CurrentOperation == EWeaponSessionOperation::Destroying)
	{
		Sessions->ClearOnDestroySessionCompleteDelegate_Handle(DestroySessionDelegateHandle);
		DestroySessionDelegateHandle.Reset();
		SetOperation(EWeaponSessionOperation::Idle);
		FailPendingAction(TEXT("DestroySession request was rejected."));
	}

	return bRequestStarted;
}

void UWeaponSessionSubsystem::HandleDestroySessionComplete(const FName SessionName, const bool bWasSuccessful)
{
	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid() && DestroySessionDelegateHandle.IsValid())
	{
		Sessions->ClearOnDestroySessionCompleteDelegate_Handle(DestroySessionDelegateHandle);
		DestroySessionDelegateHandle.Reset();
	}

	SetOperation(EWeaponSessionOperation::Idle);
	const bool bSuccess = bWasSuccessful && SessionName == WeaponSession::GameSessionName;
	OnDestroyComplete.Broadcast(bSuccess, bSuccess ? TEXT("Session destroyed.") : TEXT("Failed to destroy Session."));

	if (bSuccess)
	{
		ExecutePendingAction();
	}
	else
	{
		FailPendingAction(TEXT("Pending Session operation was cancelled because destroy failed."));
	}
}

void UWeaponSessionSubsystem::ExecutePendingAction()
{
	const EPendingAction Action = PendingAction;
	PendingAction = EPendingAction::None;

	switch (Action)
	{
	case EPendingAction::Host:
		PendingAction = EPendingAction::Host;
		BeginHostTravel();
		break;
	case EPendingAction::JoinSearchResult:
		BeginJoinSession(PendingJoinResultIndex);
		break;
	case EPendingAction::DirectTravel:
		BeginDirectTravel(PendingDirectAddress);
		break;
	case EPendingAction::RecoveryHost:
		PendingAction = EPendingAction::RecoveryHost;
		BeginRecoveryHostTravel();
		break;
	case EPendingAction::RecoveryJoinSearchResult:
		bJoiningRecoverySession = true;
		BeginJoinSession(PendingJoinResultIndex);
		break;
	case EPendingAction::RecoveryLobby:
		PendingAction = EPendingAction::RecoveryLobby;
		BeginRecoveryLobbyTravel();
		break;
	case EPendingAction::None:
	default:
		break;
	}
}

void UWeaponSessionSubsystem::FailPendingAction(const FString& Message)
{
	const EPendingAction FailedAction = PendingAction;
	PendingAction = EPendingAction::None;
	SetOperation(EWeaponSessionOperation::Idle);

	if (FailedAction == EPendingAction::Host)
	{
		BroadcastHostComplete(false, Message);
	}
	else if (FailedAction == EPendingAction::JoinSearchResult || FailedAction == EPendingAction::DirectTravel)
	{
		OnJoinComplete.Broadcast(false, Message);
	}
	else if (FailedAction == EPendingAction::RecoveryHost)
	{
		OnRecoveryWorldPrepared.Broadcast(false, Message);
	}
	else if (FailedAction == EPendingAction::RecoveryJoinSearchResult)
	{
		bJoiningRecoverySession = false;
		OnRecoveryJoinComplete.Broadcast(false, Message);
	}
}

void UWeaponSessionSubsystem::BroadcastHostComplete(
	const bool bWasSuccessful,
	const FString& Message)
{
	OnLanHostComplete.Broadcast(bWasSuccessful, Message);
	OnHostComplete.Broadcast(bWasSuccessful, Message);
}

void UWeaponSessionSubsystem::ResetRecoveryOperationFlags()
{
	bCreatingRecoverySession = false;
	bFindingRecoverySession = false;
	bJoiningRecoverySession = false;
	bHasSelectedRecoverySession = false;
	ExpectedRecoverySession = FWeaponRecoverySessionDescriptor();
	SelectedRecoverySession = FWeaponRecoverySessionDescriptor();
}

void UWeaponSessionSubsystem::ClearOnlineDelegates()
{
	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (!Sessions.IsValid())
	{
		return;
	}

	if (CreateSessionDelegateHandle.IsValid())
	{
		Sessions->ClearOnCreateSessionCompleteDelegate_Handle(CreateSessionDelegateHandle);
		CreateSessionDelegateHandle.Reset();
	}
	if (FindSessionsDelegateHandle.IsValid())
	{
		Sessions->ClearOnFindSessionsCompleteDelegate_Handle(FindSessionsDelegateHandle);
		FindSessionsDelegateHandle.Reset();
	}
	if (JoinSessionDelegateHandle.IsValid())
	{
		Sessions->ClearOnJoinSessionCompleteDelegate_Handle(JoinSessionDelegateHandle);
		JoinSessionDelegateHandle.Reset();
	}
	if (DestroySessionDelegateHandle.IsValid())
	{
		Sessions->ClearOnDestroySessionCompleteDelegate_Handle(DestroySessionDelegateHandle);
		DestroySessionDelegateHandle.Reset();
	}
}
