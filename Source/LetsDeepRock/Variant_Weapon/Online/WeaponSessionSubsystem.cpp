// Copyright Epic Games, Inc. All Rights Reserved.

#include "Online/WeaponSessionSubsystem.h"

#include "Engine/EngineBaseTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Kismet/GameplayStatics.h"
#include "OnlineSubsystemUtils.h"
#include "UObject/UObjectGlobals.h"

DEFINE_LOG_CATEGORY_STATIC(LogWeaponSession, Log, All);

namespace WeaponSession
{
	const FName GameSessionName(TEXT("GameSession"));
	const FName MapNameSetting(TEXT("MAPNAME"));
	const FName ServerNameSetting(TEXT("WEAPON_SERVER_NAME"));
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

bool UWeaponSessionSubsystem::HostLanGame(const int32 MaxPlayers, const FString& ServerName)
{
	if (CurrentOperation != EWeaponSessionOperation::Idle || MaxPlayers < 1 || MaxPlayers > 4)
	{
		return false;
	}

	PendingHostMapName = ResolveCurrentMapName();
	if (PendingHostMapName.IsNone())
	{
		OnHostComplete.Broadcast(false, TEXT("Unable to resolve the current map."));
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

void UWeaponSessionSubsystem::HandlePostLoadMap(UWorld* LoadedWorld)
{
	if (!LoadedWorld || LoadedWorld->GetGameInstance() != GetGameInstance() ||
		PendingAction != EPendingAction::Host)
	{
		return;
	}

	if (PostLoadMapDelegateHandle.IsValid())
	{
		FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapDelegateHandle);
		PostLoadMapDelegateHandle.Reset();
	}

	BeginCreateSession();
}

bool UWeaponSessionSubsystem::BeginCreateSession()
{
	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (!Sessions.IsValid())
	{
		FailPendingAction(TEXT("Online Session interface is unavailable."));
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
	Settings.NumPublicConnections = PendingHostMaxPlayers;
	Settings.NumPrivateConnections = 0;
	Settings.Set(WeaponSession::MapNameSetting, PendingHostMapName.ToString(), EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);
	Settings.Set(WeaponSession::ServerNameSetting, PendingHostServerName, EOnlineDataAdvertisementType::ViaOnlineServiceAndPing);

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
		OnHostComplete.Broadcast(false, TEXT("CreateSession request was rejected."));
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
	OnHostComplete.Broadcast(bSuccess, bSuccess ? TEXT("LAN host is listening and advertised.") : TEXT("Failed to create LAN Session."));
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

void UWeaponSessionSubsystem::HandleFindSessionsComplete(const bool bWasSuccessful)
{
	const IOnlineSessionPtr Sessions = GetSessionInterface();
	if (Sessions.IsValid() && FindSessionsDelegateHandle.IsValid())
	{
		Sessions->ClearOnFindSessionsCompleteDelegate_Handle(FindSessionsDelegateHandle);
		FindSessionsDelegateHandle.Reset();
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

bool UWeaponSessionSubsystem::JoinLanGame(const int32 ResultIndex)
{
	if (CurrentOperation != EWeaponSessionOperation::Idle || !ActiveSearch.IsValid() ||
		!ActiveSearch->SearchResults.IsValidIndex(ResultIndex))
	{
		return false;
	}

	PendingJoinResultIndex = ResultIndex;
	PendingAction = EPendingAction::JoinSearchResult;

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
		OnJoinComplete.Broadcast(false, TEXT("JoinSession request was rejected."));
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
		OnJoinComplete.Broadcast(false, FString::Printf(TEXT("Join failed: %s"), LexToString(Result)));
		return;
	}

	FString ConnectString;
	if (!Sessions->GetResolvedConnectString(WeaponSession::GameSessionName, ConnectString))
	{
		SetOperation(EWeaponSessionOperation::Idle);
		OnJoinComplete.Broadcast(false, TEXT("The joined Session has no valid game address."));
		return;
	}

	SetOperation(EWeaponSessionOperation::DirectTravel);
	if (APlayerController* PlayerController = GetGameInstance()->GetFirstLocalPlayerController(GetWorld()))
	{
		PlayerController->ClientTravel(ConnectString, TRAVEL_Absolute);
		SetOperation(EWeaponSessionOperation::Idle);
		OnJoinComplete.Broadcast(true, ConnectString);
	}
	else
	{
		SetOperation(EWeaponSessionOperation::Idle);
		OnJoinComplete.Broadcast(false, TEXT("No local PlayerController is available for travel."));
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
		OnHostComplete.Broadcast(false, Message);
	}
	else if (FailedAction == EPendingAction::JoinSearchResult || FailedAction == EPendingAction::DirectTravel)
	{
		OnJoinComplete.Broadcast(false, Message);
	}
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
