// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponCentralConnectivitySubsystem.h"

#include "WeaponCentralDeviceCrypto.h"
#include "WeaponCentralConnectivitySubsystem.h"
#include "WeaponHostMigrationSubsystem.h"
#include "WeaponRelayNetDriver.h"
#include "WeaponSessionSubsystem.h"
#include "WeaponSnapshotCoordinator.h"
#include "WeaponStunClient.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "PlatformHttp.h"
#include "IWebSocket.h"
#include "WebSocketsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "SocketSubsystem.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Base64.h"
#include "TimerManager.h"
#include "UObject/Package.h"

DEFINE_LOG_CATEGORY_STATIC(LogWeaponCentralConnectivity, Log, All);

void UWeaponCentralConnectivitySubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// 经 GameInstance 解析兄弟子系统(GetSubsystem<T> 在运行时可用,
	// 但 Initialize 参数是基类 FSubsystemCollectionBase,没有模板方法)。
	if (UGameInstance* GI = GetGameInstance())
	{
		HostMigration = GI->GetSubsystem<UWeaponHostMigrationSubsystem>();
		SessionSubsystem = GI->GetSubsystem<UWeaponSessionSubsystem>();
		SnapshotCoordinator = GI->GetSubsystem<UWeaponSnapshotCoordinator>();
		if (SessionSubsystem.IsValid())
		{
			SessionSubsystem->OnLanHostComplete.AddUObject(
				this,
				&UWeaponCentralConnectivitySubsystem::HandleLanHostComplete);
		}
	}

	ConnectivityState = EWeaponCentralConnectivityState::Disconnected;
	UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Central Connectivity subsystem initialized"));
}

void UWeaponCentralConnectivitySubsystem::Deinitialize()
{
	if (SessionSubsystem.IsValid())
	{
		SessionSubsystem->OnLanHostComplete.RemoveAll(this);
	}
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(ReconnectTimerHandle);
		GI->GetTimerManager().ClearTimer(LeaseHeartbeatTimerHandle);
		GI->GetTimerManager().ClearTimer(HostEndpointPollTimerHandle);
		GI->GetTimerManager().ClearTimer(HostEndpointWaitTimerHandle);
		GI->GetTimerManager().ClearTimer(RelayTickTimerHandle);
	}
	ResetEventSocket();
	ConnectivityState = EWeaponCentralConnectivityState::Disconnected;
	Super::Deinitialize();
}

void UWeaponCentralConnectivitySubsystem::StartCentralConnect()
{
	if (ConnectivityState == EWeaponCentralConnectivityState::Authenticating ||
		ConnectivityState == EWeaponCentralConnectivityState::Connected)
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("StartCentralConnect: already active (%d)"), (int32)ConnectivityState);
		return;
	}

	AuthState = EWeaponCentralAuthState::None;
	if (FString ConfigError; !NormalizeAndValidateCoordinatorBaseUrl(ConfigError))
	{
		FailAuthPipeline(ConfigError);
		return;
	}

	ConnectivityState = EWeaponCentralConnectivityState::Authenticating;
	OnConnectivityChanged.Broadcast(ConnectivityState, TEXT("Starting central authentication"));

	// RunAuthPipeline() 发起 challenge → register → session。
	// 适配器不在这里做任何决策,只把事实交给 HostMigration。
	RunAuthPipeline();
}

bool UWeaponCentralConnectivitySubsystem::NormalizeAndValidateCoordinatorBaseUrl(FString& OutError)
{
	CoordinatorBaseUrl.TrimStartAndEndInline();
	while (CoordinatorBaseUrl.EndsWith(TEXT("/")))
	{
		CoordinatorBaseUrl.LeftChopInline(1);
	}

	if (!CoordinatorBaseUrl.StartsWith(TEXT("http://"), ESearchCase::IgnoreCase) &&
		!CoordinatorBaseUrl.StartsWith(TEXT("https://"), ESearchCase::IgnoreCase))
	{
		OutError = FString::Printf(
			TEXT("Invalid CoordinatorBaseUrl '%s': expected http:// or https:// URL"),
			*CoordinatorBaseUrl);
		return false;
	}

	for (const TCHAR Character : CoordinatorBaseUrl)
	{
		if (FChar::IsWhitespace(Character))
		{
			OutError = FString::Printf(
				TEXT("Invalid CoordinatorBaseUrl '%s': whitespace is not allowed"),
				*CoordinatorBaseUrl);
			return false;
		}
	}

	if (FPlatformHttp::GetUrlDomain(CoordinatorBaseUrl).IsEmpty())
	{
		OutError = FString::Printf(
			TEXT("Invalid CoordinatorBaseUrl '%s': URL has no host"),
			*CoordinatorBaseUrl);
		return false;
	}

	UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Coordinator base URL: %s"), *CoordinatorBaseUrl);
	return true;
}

bool UWeaponCentralConnectivitySubsystem::UploadCandidate(const FWeaponCentralEndpointCandidate& Candidate, FString& OutError) const
{
	if (!MatchId.IsValid() || CentralMatchIdString.IsEmpty())
	{
		OutError = TEXT("No active MatchId; cannot upload candidate");
		return false;
	}
	if (AccessToken.IsEmpty())
	{
		OutError = TEXT("Not authenticated; cannot upload candidate");
		return false;
	}

	FHttpModule& Http = FHttpModule::Get();
	const TSharedRef<IHttpRequest> Request = Http.CreateRequest();
	Request->SetURL(FString::Printf(
		TEXT("%s/v1/matches/%s/peers/%s/candidates"),
		*CoordinatorBaseUrl, *CentralMatchIdString, *Candidate.PeerId));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Connection"), TEXT("close"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));

	const FString Body = FString::Printf(
		TEXT("{\"authorityEpoch\":%llu,\"rosterRevision\":%llu,\"peerId\":\"%s\",\"candidateId\":\"%s\",\"transport\":\"%s\",\"candidateType\":\"%s\",\"address\":\"%s\",\"port\":%d,\"priority\":%u,\"foundation\":\"%s\"}"),
		Candidate.AuthorityEpoch, Candidate.RosterRevision,
		*Candidate.PeerId, *Candidate.CandidateId,
		*Candidate.Transport, *Candidate.CandidateType,
		*Candidate.Address, Candidate.Port, Candidate.Priority, *Candidate.Foundation);
	Request->SetContentAsString(Body);

	Request->OnProcessRequestComplete().BindLambda([this](FHttpRequestPtr Req, const FHttpResponsePtr& Resp, bool bWasSuccessful)
	{
		if (!bWasSuccessful || !Resp.IsValid() || Resp->GetResponseCode() != 201)
		{
			UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("UploadCandidate failed (HTTP %d)"),
				Resp.IsValid() ? Resp->GetResponseCode() : 0);
			return;
		}
		UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Candidate uploaded to central"));
	});
	Request->ProcessRequest();
	return true;
}

bool UWeaponCentralConnectivitySubsystem::CreateCentralMatch(FString& OutError)
{
	CentralJoinCode.Reset();
	if (AccessToken.IsEmpty())
	{
		// 认证尚未完成:排队,HandleSessionSuccess 后执行
		PendingMatchAction = EPendingMatchAction::Create;
		PendingJoinCode.Reset();
		return true;
	}
	SendCreateMatchRequest();
	return true;
}

bool UWeaponCentralConnectivitySubsystem::JoinCentralMatch(const FString& JoinCode, FString& OutError)
{
	const FString NormalizedJoinCode = JoinCode.TrimStartAndEnd();
	if (NormalizedJoinCode.IsEmpty())
	{
		OutError = TEXT("Empty join code");
		return false;
	}
	CentralJoinCode.Reset();
	if (AccessToken.IsEmpty())
	{
		// 认证尚未完成:排队,HandleSessionSuccess 后执行
		PendingMatchAction = EPendingMatchAction::Join;
		PendingJoinCode = NormalizedJoinCode;
		return true;
	}
	SendJoinMatchRequest(NormalizedJoinCode);
	return true;
}

void UWeaponCentralConnectivitySubsystem::ExecutePendingMatchAction()
{
	switch (PendingMatchAction)
	{
	case EPendingMatchAction::Create:
		PendingMatchAction = EPendingMatchAction::None;
		SendCreateMatchRequest();
		break;
	case EPendingMatchAction::Join:
	{
		const FString JoinCode = PendingJoinCode;
		PendingMatchAction = EPendingMatchAction::None;
		PendingJoinCode.Reset();
		SendJoinMatchRequest(JoinCode);
		break;
	}
	default:
		break;
	}
}

void UWeaponCentralConnectivitySubsystem::SendCreateMatchRequest()
{
	FHttpModule& Http = FHttpModule::Get();
	const TSharedRef<IHttpRequest> Request = Http.CreateRequest();
	Request->SetURL(CoordinatorBaseUrl + TEXT("/v1/matches"));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Connection"), TEXT("close"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));
	Request->SetContentAsString(TEXT("{}"));

	Request->OnProcessRequestComplete().BindLambda([this](FHttpRequestPtr Req, const FHttpResponsePtr& Resp, bool bWasSuccessful)
	{
		if (!bWasSuccessful || !Resp.IsValid() || Resp->GetResponseCode() != 201)
		{
			const int32 StatusCode = Resp.IsValid() ? Resp->GetResponseCode() : 0;
			const FString ResponseBody = Resp.IsValid() ? Resp->GetContentAsString() : FString();
			FailMatchAction(FString::Printf(
				TEXT("Create Match failed (HTTP %d): %s"), StatusCode, *ResponseBody));
			return;
		}

		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp->GetContentAsString());
		if (!FJsonSerializer::Deserialize(Reader, Json))
		{
			FailMatchAction(TEXT("Create Match response parse failed"));
			return;
		}

		const FString RawMatchId = Json->GetStringField(TEXT("matchId"));
		const FGuid CreatedMatchId(RawMatchId);
		const FString JoinCode = Json->GetStringField(TEXT("joinCode"));
		FString HostPeerId;
		Json->TryGetStringField(TEXT("hostDeviceId"), HostPeerId);
		const uint64 MatchEpoch = static_cast<uint64>(Json->GetNumberField(TEXT("authorityEpoch")));
		const uint64 MatchRosterRevision = static_cast<uint64>(Json->GetNumberField(TEXT("rosterRevision")));
		HandleMatchCreated(
			CreatedMatchId,
			RawMatchId,
			JoinCode,
			HostPeerId,
			MatchEpoch,
			MatchRosterRevision);
	});
	Request->ProcessRequest();
}

void UWeaponCentralConnectivitySubsystem::FailMatchAction(const FString& Message)
{
	StopHostEndpointWait();
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(LeaseHeartbeatTimerHandle);
	}
	PendingMatchAction = EPendingMatchAction::None;
	PendingJoinCode.Reset();
	ConnectivityState = EWeaponCentralConnectivityState::Failed;
	UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("Central Match action failed: %s"), *Message);
	OnConnectivityChanged.Broadcast(ConnectivityState, Message);
}

void UWeaponCentralConnectivitySubsystem::SendJoinMatchRequest(const FString& JoinCode)
{
	FHttpModule& Http = FHttpModule::Get();
	const TSharedRef<IHttpRequest> Request = Http.CreateRequest();
	Request->SetURL(CoordinatorBaseUrl + TEXT("/v1/matches/join"));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Connection"), TEXT("close"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));
	Request->SetContentAsString(FString::Printf(TEXT("{\"joinCode\":\"%s\"}"), *JoinCode));

	Request->OnProcessRequestComplete().BindLambda([this, JoinCode](FHttpRequestPtr Req, const FHttpResponsePtr& Resp, bool bWasSuccessful)
	{
		if (!bWasSuccessful || !Resp.IsValid() || Resp->GetResponseCode() != 200)
		{
			FailMatchAction(FString::Printf(TEXT("Join Match failed (HTTP %d): %s"),
				Resp.IsValid() ? Resp->GetResponseCode() : 0,
				Resp.IsValid() ? *Resp->GetContentAsString() : TEXT("(no response)")));
			return;
		}

		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp->GetContentAsString());
		if (!FJsonSerializer::Deserialize(Reader, Json))
		{
			FailMatchAction(TEXT("Join Match response parse failed"));
			return;
		}

		const TSharedPtr<FJsonObject>* MatchObj = nullptr;
		if (!Json->TryGetObjectField(TEXT("match"), MatchObj) || !MatchObj->IsValid())
		{
			FailMatchAction(TEXT("Join Match response missing match"));
			return;
		}

		// 保留原始 matchId 字符串(Go uuid 格式),不经 FGuid 往返
		const FString RawMatchId = (*MatchObj)->GetStringField(TEXT("matchId"));
		const FGuid JoinedMatchId(RawMatchId);
		const FString Role = Json->GetStringField(TEXT("role"));
		FString HostPeerId;
		(*MatchObj)->TryGetStringField(TEXT("hostDeviceId"), HostPeerId);
		const uint64 MatchEpoch = static_cast<uint64>((*MatchObj)->GetNumberField(TEXT("authorityEpoch")));
		const uint64 MatchRosterRevision = static_cast<uint64>((*MatchObj)->GetNumberField(TEXT("rosterRevision")));
		HandleMatchJoined(
			JoinedMatchId,
			RawMatchId,
			Role,
			JoinCode,
			HostPeerId,
			MatchEpoch,
			MatchRosterRevision);
	});
	Request->ProcessRequest();
}

void UWeaponCentralConnectivitySubsystem::HandleMatchCreated(
	const FGuid& InMatchId,
	const FString& RawMatchId,
	const FString& JoinCode,
	const FString& HostPeerId,
	const uint64 InAuthorityEpoch,
	const uint64 InRosterRevision)
{
	if (!InMatchId.IsValid() || RawMatchId.IsEmpty() || JoinCode.IsEmpty())
	{
		FailMatchAction(TEXT("Create Match returned an invalid MatchId or JoinCode"));
		return;
	}

	if (!SnapshotCoordinator.IsValid())
	{
		FailMatchAction(TEXT("Snapshot Coordinator is unavailable"));
		return;
	}

	FString AdoptError;
	if (!SnapshotCoordinator->AdoptActiveMatchId(InMatchId, AdoptError))
	{
		FailMatchAction(FString::Printf(TEXT("Cannot adopt central MatchId: %s"), *AdoptError));
		return;
	}

	ResetMatchTransportState();
	MatchId = InMatchId;
	CentralMatchIdString = RawMatchId;
	CentralJoinCode = JoinCode;
	CurrentMatchRole = TEXT("Host");
	ExpectedHostPeerId = HostPeerId.IsEmpty() ? DeviceId : HostPeerId;
	AuthorityEpoch = InAuthorityEpoch;
	RosterRevision = InRosterRevision;
	bCreatedCentralMatch = true;
	UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Central match created: %s (join code %s)"),
		*CentralMatchIdString, *JoinCode);

	// MatchId 建立 → 重放暂存事件 + 打开事件通道
	ReplayStashedEvents();
	OpenEventChannel();

	// Host 也预取 relay 凭证:自己的 TURN 分配(relayed 候选)与后续
	// 迁移都依赖它。失败不阻塞,relay 只是附加能力。
	// (凭证到达后由 RequestRelayCredentials 回调内配置 Host NetDriver)
	FString RelayError;
	if (IsCentralMatchReady())
	{
		RequestRelayCredentials(RelayError);
	}

	// 创建 Host 的 UI 只需要控制面认证 + MatchId。Lease / 首个 Checkpoint
	// 必须进入玩法世界后才能继续收敛，不能反向阻塞 Host 创建。
	ConnectivityState = EWeaponCentralConnectivityState::Connected;
	OnConnectivityChanged.Broadcast(ConnectivityState, TEXT("Central Match ready"));
}

void UWeaponCentralConnectivitySubsystem::HandleMatchJoined(
	const FGuid& InMatchId,
	const FString& RawMatchId,
	const FString& Role,
	const FString& JoinCode,
	const FString& HostPeerId,
	const uint64 InAuthorityEpoch,
	const uint64 InRosterRevision)
{
	if (!InMatchId.IsValid() || RawMatchId.IsEmpty() || JoinCode.IsEmpty() || HostPeerId.IsEmpty())
	{
		FailMatchAction(TEXT("Join Match returned an invalid MatchId, JoinCode, or Host identity"));
		return;
	}

	if (!SnapshotCoordinator.IsValid())
	{
		FailMatchAction(TEXT("Snapshot Coordinator is unavailable"));
		return;
	}

	FString AdoptError;
	if (!SnapshotCoordinator->AdoptActiveMatchId(InMatchId, AdoptError))
	{
		FailMatchAction(FString::Printf(TEXT("Cannot adopt central MatchId: %s"), *AdoptError));
		return;
	}

	ResetMatchTransportState();
	MatchId = InMatchId;
	CentralMatchIdString = RawMatchId;
	CentralJoinCode = JoinCode;
	CurrentMatchRole = Role;
	ExpectedHostPeerId = HostPeerId;
	AuthorityEpoch = InAuthorityEpoch;
	RosterRevision = InRosterRevision;
	bCreatedCentralMatch = false;
	UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Central match joined: %s (role %s)"),
		*CentralMatchIdString, *Role);

	// MatchId 建立 → 重放暂存事件 + 打开事件通道
	ReplayStashedEvents();
	OpenEventChannel();

	// 预取 relay 凭证:direct 不可达时 ConfigureRelayNetDriver 立即可用。
	// 失败不阻塞加入流程,relay 只是 fallback。
	FString RelayError;
	if (IsCentralMatchReady())
	{
		RequestRelayCredentials(RelayError);
	}

	ConnectivityState = EWeaponCentralConnectivityState::Connected;
	OnConnectivityChanged.Broadcast(ConnectivityState, TEXT("Central Match joined; waiting for host endpoint"));
	BeginHostEndpointWait();
}

void UWeaponCentralConnectivitySubsystem::ResetMatchTransportState()
{
	StopHostEndpointWait();
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(LeaseHeartbeatTimerHandle);
	}
	ResetEventSocket();
	bCentralLeaseReady = false;
	bCentralCheckpointReady = false;
	bCreatedCentralMatch = false;
	bLeaseClaimInFlight = false;
	bCandidateRequestInFlight = false;
	PublishedCandidateId.Reset();
	ExpectedHostPeerId.Reset();
	CurrentMatchRole.Reset();
}

void UWeaponCentralConnectivitySubsystem::HandleLanHostComplete(
	const bool bWasSuccessful,
	const FString& Message)
{
	if (!bCreatedCentralMatch || CurrentMatchRole != TEXT("Host"))
	{
		return;
	}
	if (!bWasSuccessful)
	{
		FailMatchAction(FString::Printf(TEXT("Listen Host failed before endpoint publication: %s"), *Message));
		return;
	}

	ClaimHostLease();
}

void UWeaponCentralConnectivitySubsystem::ClaimHostLease()
{
	if (bLeaseClaimInFlight || !bCreatedCentralMatch || !MatchId.IsValid() ||
		CentralMatchIdString.IsEmpty() || DeviceId.IsEmpty() || AccessToken.IsEmpty())
	{
		return;
	}

	bLeaseClaimInFlight = true;
	const uint64 ExpectedEpoch = AuthorityEpoch + 1;
	FHttpModule& Http = FHttpModule::Get();
	const TSharedRef<IHttpRequest> Request = Http.CreateRequest();
	Request->SetURL(FString::Printf(
		TEXT("%s/v1/matches/%s/lease/claim"),
		*CoordinatorBaseUrl,
		*CentralMatchIdString));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Connection"), TEXT("close"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));
	Request->SetContentAsString(FString::Printf(
		TEXT("{\"peerId\":\"%s\",\"expectedEpoch\":%llu}"),
		*DeviceId,
		ExpectedEpoch));

	Request->OnProcessRequestComplete().BindLambda(
		[this](FHttpRequestPtr Req, const FHttpResponsePtr& Resp, const bool bWasSuccessful)
		{
			bLeaseClaimInFlight = false;
			if (!bWasSuccessful || !Resp.IsValid() || Resp->GetResponseCode() != 201)
			{
				FailMatchAction(FString::Printf(
					TEXT("Host Lease claim failed (HTTP %d): %s"),
					Resp.IsValid() ? Resp->GetResponseCode() : 0,
					Resp.IsValid() ? *Resp->GetContentAsString() : TEXT("(no response)")));
				return;
			}

			TSharedPtr<FJsonObject> Json;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Resp->GetContentAsString());
			if (!FJsonSerializer::Deserialize(Reader, Json))
			{
				FailMatchAction(TEXT("Host Lease response parse failed"));
				return;
			}

			FString LeaseHostPeerId;
			Json->TryGetStringField(TEXT("hostPeerId"), LeaseHostPeerId);
			if (!LeaseHostPeerId.IsEmpty())
			{
				ExpectedHostPeerId = LeaseHostPeerId;
			}
			FWeaponCentralHostLease Lease;
			Lease.MatchId = MatchId;
			Lease.AuthorityEpoch = static_cast<uint64>(Json->GetNumberField(TEXT("authorityEpoch")));
			HandleLeaseGranted(Lease);
			PublishHostEndpoint();

			if (UGameInstance* GI = GetGameInstance())
			{
				GI->GetTimerManager().SetTimer(
					LeaseHeartbeatTimerHandle,
					this,
					&UWeaponCentralConnectivitySubsystem::SendLeaseHeartbeat,
					FMath::Max(1.0f, LeaseHeartbeatIntervalSeconds),
					true);
			}
		});
	Request->ProcessRequest();
}

void UWeaponCentralConnectivitySubsystem::SendLeaseHeartbeat()
{
	if (!bCreatedCentralMatch || !bCentralLeaseReady || AccessToken.IsEmpty() ||
		CentralMatchIdString.IsEmpty() || AuthorityEpoch == 0)
	{
		return;
	}

	FHttpModule& Http = FHttpModule::Get();
	const TSharedRef<IHttpRequest> Request = Http.CreateRequest();
	Request->SetURL(FString::Printf(
		TEXT("%s/v1/matches/%s/lease/heartbeat"),
		*CoordinatorBaseUrl,
		*CentralMatchIdString));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Connection"), TEXT("close"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));
	Request->SetContentAsString(FString::Printf(
		TEXT("{\"peerId\":\"%s\",\"expectedEpoch\":%llu}"),
		*DeviceId,
		AuthorityEpoch));
	Request->OnProcessRequestComplete().BindLambda(
		[this](FHttpRequestPtr Req, const FHttpResponsePtr& Resp, const bool bWasSuccessful)
		{
			if (!bWasSuccessful || !Resp.IsValid() || Resp->GetResponseCode() != 200)
			{
				bCentralLeaseReady = false;
				UE_LOG(
					LogWeaponCentralConnectivity,
					Warning,
					TEXT("Host Lease heartbeat failed (HTTP %d)"),
					Resp.IsValid() ? Resp->GetResponseCode() : 0);
				PublishBackendReadiness();
				return;
			}

			// Candidate rows expire after two minutes. Refreshing on every heartbeat
			// keeps late JoinCode users independent of one transient WS event.
			PublishHostEndpoint();
		});
	Request->ProcessRequest();
}

bool UWeaponCentralConnectivitySubsystem::ResolveHostEndpoint(
	FString& OutAddress,
	int32& OutPort,
	FString& OutError) const
{
	OutAddress = AdvertisedHostAddress.TrimStartAndEnd();
	OutPort = AdvertisedHostPort;
	FParse::Value(FCommandLine::Get(), TEXT("WeaponCentralAdvertisedHost="), OutAddress);
	FParse::Value(FCommandLine::Get(), TEXT("WeaponCentralAdvertisedPort="), OutPort);
	OutAddress.TrimStartAndEndInline();

	UWorld* World = GetWorld();
	UNetDriver* NetDriver = World ? World->GetNetDriver() : nullptr;
	if (!World || !NetDriver || World->GetNetMode() != NM_ListenServer)
	{
		OutError = TEXT("The gameplay IpNetDriver is not listening");
		return false;
	}

	if (OutPort <= 0)
	{
		const TSharedPtr<const FInternetAddr> LocalAddr = NetDriver->GetLocalAddr();
		OutPort = LocalAddr.IsValid() ? LocalAddr->GetPort() : World->URL.Port;
	}
	if (OutPort <= 0 || OutPort > 65535)
	{
		OutError = FString::Printf(TEXT("Invalid listen port %d"), OutPort);
		return false;
	}

	if (OutAddress.IsEmpty())
	{
		ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
		if (!SocketSubsystem)
		{
			OutError = TEXT("Socket subsystem is unavailable");
			return false;
		}
		bool bCanBindAll = false;
		OutAddress = SocketSubsystem->GetLocalHostAddr(*GLog, bCanBindAll)->ToString(false);
		UE_LOG(
			LogWeaponCentralConnectivity,
			Warning,
			TEXT("AdvertisedHostAddress is empty; publishing LAN address %s. Cross-NAT join requires a public address/port mapping or a relay."),
			*OutAddress);
	}

	if (OutAddress.IsEmpty() || OutAddress == TEXT("0.0.0.0") || OutAddress == TEXT("::"))
	{
		OutError = TEXT("No routable Host address is available");
		return false;
	}
	return true;
}

void UWeaponCentralConnectivitySubsystem::PublishHostEndpoint()
{
	if (!bCreatedCentralMatch || !bCentralLeaseReady || ExpectedHostPeerId.IsEmpty())
	{
		return;
	}

	FString Address;
	int32 Port = 0;
	FString Error;
	if (!ResolveHostEndpoint(Address, Port, Error))
	{
		FailMatchAction(FString::Printf(TEXT("Cannot publish Host endpoint: %s"), *Error));
		return;
	}

	// 有 relay 凭证时,Host 的 Listen socket 应已走 TURN(仅经 TURN 可达)。
	// 若 relayed 地址尚未就绪(TURN Allocate 未完成),降级发布 direct 候选,
	// 保证客户端始终能拿到候选;heartbeat 周期会补发 relayed。
	if (HasRelayCredentials())
	{
		const FString RelayedAddr = GetRelayedHostCandidateAddress();
		if (!RelayedAddr.IsEmpty())
		{
			PublishRelayedHostCandidate();
			return;
		}
		UE_LOG(LogWeaponCentralConnectivity, Warning,
			TEXT("Relay credentials ready but relayed address not yet allocated; publishing direct candidates as fallback"));
	}

	// 内网/手动配置的 direct 候选(始终发布;同 NAT 客户端可直接连)。
	UploadHostCandidate(TEXT("Host"), Address, Port, 100, TEXT("listen"));

	// STUN 反射候选:本机在公网的地址。UDP 直连不经过 libcurl/代理,
	// 拿到的是真实出口(而非代理出口)。失败不阻塞,继续用内网候选。
	FString ReflexiveAddress;
	int32 ReflexivePort = 0;
	QueryStunReflexiveAddress(ReflexiveAddress, ReflexivePort);
	if (!ReflexiveAddress.IsEmpty() && ReflexivePort > 0)
	{
		UploadHostCandidate(TEXT("ServerReflexive"), ReflexiveAddress, ReflexivePort, 90, TEXT("stun"));
		UE_LOG(LogWeaponCentralConnectivity, Log,
			TEXT("Published server-reflexive candidate %s:%d"), *ReflexiveAddress, ReflexivePort);
	}

	// 向中心提名最高优先级的 direct endpoint(Host 候选;STUN 反射候选
	// 仅是附加,不改变既有 HostEndpointPublished 事件语义)。
	NominateHostEndpoint(Address, Port);
}

FString UWeaponCentralConnectivitySubsystem::GetRelayedHostCandidateAddress() const
{
	UWorld* World = GetWorld();
	UNetDriver* NetDriver = World ? World->GetNetDriver() : nullptr;
	UWeaponRelayNetDriver* RelayDriver = Cast<UWeaponRelayNetDriver>(NetDriver);
	if (!RelayDriver)
	{
		return FString();
	}
	return RelayDriver->GetRelayedAddressString();
}

void UWeaponCentralConnectivitySubsystem::PublishRelayedHostCandidate()
{
	const FString RelayedAddr = GetRelayedHostCandidateAddress();
	if (RelayedAddr.IsEmpty())
	{
		return;
	}

	FString RelayedHost;
	int32 RelayedPort = 0;
	const int32 Colon = RelayedAddr.Find(TEXT(":"));
	if (Colon == INDEX_NONE)
	{
		RelayedHost = RelayedAddr;
	}
	else
	{
		RelayedHost = RelayedAddr.Left(Colon);
		RelayedPort = FCString::Atoi(*RelayedAddr.RightChop(Colon + 1));
	}
	if (RelayedHost.IsEmpty() || RelayedPort <= 0)
	{
		return;
	}

	FWeaponCentralEndpointCandidate Candidate;
	Candidate.MatchId = CentralMatchIdString;
	Candidate.AuthorityEpoch = AuthorityEpoch;
	Candidate.RosterRevision = RosterRevision;
	Candidate.PeerId = ExpectedHostPeerId;
	Candidate.CandidateId = PublishedCandidateId + TEXT("-Relay");
	Candidate.Transport = TEXT("Relay");
	Candidate.CandidateType = TEXT("Relayed");
	Candidate.Address = RelayedHost;
	Candidate.Port = RelayedPort;
	Candidate.Priority = 80;
	Candidate.Foundation = TEXT("relay");

	FString Error;
	if (!UploadCandidate(Candidate, Error))
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning,
			TEXT("Cannot upload relayed candidate %s: %s"), *RelayedAddr, *Error);
		return;
	}
	UE_LOG(LogWeaponCentralConnectivity, Log,
		TEXT("Published relayed candidate %s"), *RelayedAddr);
}

void UWeaponCentralConnectivitySubsystem::UploadHostCandidate(
	const FString& CandidateType,
	const FString& Address,
	const int32 Port,
	const uint32 Priority,
	const FString& Foundation)
{
	if (CandidateType.IsEmpty() || Address.IsEmpty() || Port <= 0 || Port > 65535)
	{
		return;
	}

	if (PublishedCandidateId.IsEmpty())
	{
		PublishedCandidateId = FGuid::NewGuid().ToString(EGuidFormats::DigitsWithHyphensLower);
	}
	FWeaponCentralEndpointCandidate Candidate;
	Candidate.MatchId = CentralMatchIdString;
	Candidate.AuthorityEpoch = AuthorityEpoch;
	Candidate.RosterRevision = RosterRevision;
	Candidate.PeerId = ExpectedHostPeerId;
	// 每类候选独立 ID:Go 端 ON CONFLICT (match_id, peer_id, candidate_id)
	// 覆盖更新,共用 ID 会导致后上传的候选覆盖先上传的。
	Candidate.CandidateId = PublishedCandidateId + TEXT("-") + CandidateType;
	Candidate.Transport = TEXT("UDP");
	Candidate.CandidateType = CandidateType;
	Candidate.Address = Address;
	Candidate.Port = Port;
	Candidate.Priority = Priority;
	Candidate.Foundation = Foundation;

	FString Error;
	if (!UploadCandidate(Candidate, Error))
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning,
			TEXT("Cannot upload %s candidate %s:%d: %s"), *CandidateType, *Address, Port, *Error);
	}
}

void UWeaponCentralConnectivitySubsystem::QueryStunReflexiveAddress(FString& OutAddress, int32& OutPort)
{
	OutAddress.Reset();
	OutPort = 0;

	if (StunServerUrl.IsEmpty())
	{
		return;
	}

	FString Host;
	int32 ServerPort = 0;
	if (!FWeaponStunClient::ParseServerUrl(StunServerUrl, Host, ServerPort))
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("Invalid StunServerUrl '%s'"), *StunServerUrl);
		return;
	}

	FWeaponStunClient Client;
	if (!Client.QueryReflexiveAddress(Host, ServerPort, OutAddress, OutPort))
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning,
			TEXT("STUN reflexive lookup failed via %s; continuing with LAN-only candidate"), *StunServerUrl);
	}
}

void UWeaponCentralConnectivitySubsystem::NominateHostEndpoint(
	const FString& Address,
	const int32 Port)
{
	FHttpModule& Http = FHttpModule::Get();
	const TSharedRef<IHttpRequest> Request = Http.CreateRequest();
	Request->SetURL(FString::Printf(
		TEXT("%s/v1/matches/%s/connectivity/nominate"),
		*CoordinatorBaseUrl,
		*CentralMatchIdString));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Connection"), TEXT("close"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));
	Request->SetContentAsString(FString::Printf(
		TEXT("{\"authorityEpoch\":%llu,\"address\":\"%s\",\"port\":%d}"),
		AuthorityEpoch,
		*Address,
		Port));
	Request->OnProcessRequestComplete().BindLambda(
		[this, Address, Port](FHttpRequestPtr Req, const FHttpResponsePtr& Resp, const bool bWasSuccessful)
		{
			if (!bWasSuccessful || !Resp.IsValid() || Resp->GetResponseCode() != 200)
			{
				UE_LOG(
					LogWeaponCentralConnectivity,
					Warning,
					TEXT("Host endpoint nomination failed (HTTP %d): %s"),
					Resp.IsValid() ? Resp->GetResponseCode() : 0,
					Resp.IsValid() ? *Resp->GetContentAsString() : TEXT("(no response)"));
				return;
			}
			UE_LOG(
				LogWeaponCentralConnectivity,
				Log,
				TEXT("Host endpoint published: %s"),
				*FormatEndpointAddress(Address, Port));
		});
	Request->ProcessRequest();
}

void UWeaponCentralConnectivitySubsystem::BeginHostEndpointWait()
{
	bWaitingForHostEndpoint = true;
	bCandidateRequestInFlight = false;
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().SetTimer(
			HostEndpointWaitTimerHandle,
			this,
			&UWeaponCentralConnectivitySubsystem::HandleHostEndpointWaitTimeout,
			FMath::Max(1.0f, HostEndpointWaitTimeoutSeconds),
			false);
	}
	RequestHostCandidates();
}

void UWeaponCentralConnectivitySubsystem::RequestHostCandidates()
{
	if (!bWaitingForHostEndpoint || bCandidateRequestInFlight || AccessToken.IsEmpty() ||
		CentralMatchIdString.IsEmpty() || DeviceId.IsEmpty())
	{
		return;
	}

	bCandidateRequestInFlight = true;
	FHttpModule& Http = FHttpModule::Get();
	const TSharedRef<IHttpRequest> Request = Http.CreateRequest();
	Request->SetURL(FString::Printf(
		TEXT("%s/v1/matches/%s/peers/%s/candidates"),
		*CoordinatorBaseUrl,
		*CentralMatchIdString,
		*DeviceId));
	Request->SetVerb(TEXT("GET"));
	Request->SetHeader(TEXT("Connection"), TEXT("close"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));
	Request->OnProcessRequestComplete().BindLambda(
		[this](FHttpRequestPtr Req, const FHttpResponsePtr& Resp, const bool bWasSuccessful)
		{
			bCandidateRequestInFlight = false;
			if (!bWaitingForHostEndpoint)
			{
				return;
			}
			if (!bWasSuccessful || !Resp.IsValid() || Resp->GetResponseCode() != 200)
			{
				UE_LOG(
					LogWeaponCentralConnectivity,
					Warning,
					TEXT("Host candidate query failed (HTTP %d)"),
					Resp.IsValid() ? Resp->GetResponseCode() : 0);
				ScheduleHostCandidatePoll();
				return;
			}

			TSharedPtr<FJsonObject> Json;
			const TSharedRef<TJsonReader<>> Reader =
				TJsonReaderFactory<>::Create(Resp->GetContentAsString());
			const TArray<TSharedPtr<FJsonValue>>* Candidates = nullptr;
			if (!FJsonSerializer::Deserialize(Reader, Json))
			{
				UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("Host candidate response parse failed"));
				ScheduleHostCandidatePoll();
				return;
			}
			// candidates 缺失或为 null(Go 端历史版本把空列表序列化为 null)
			// 都视为空列表继续轮询,而不是判定为解析失败。
			if (!Json->TryGetArrayField(TEXT("candidates"), Candidates))
			{
				ScheduleHostCandidatePoll();
				return;
			}

			FWeaponCentralEndpointCandidate Best;
			FWeaponCentralEndpointCandidate BestRelay;
			for (const TSharedPtr<FJsonValue>& Value : *Candidates)
			{
				const TSharedPtr<FJsonObject> CandidateJson = Value.IsValid() ? Value->AsObject() : nullptr;
				if (!CandidateJson.IsValid())
				{
					continue;
				}
				FWeaponCentralEndpointCandidate Candidate;
				CandidateJson->TryGetStringField(TEXT("peerId"), Candidate.PeerId);
				CandidateJson->TryGetStringField(TEXT("candidateType"), Candidate.CandidateType);
				CandidateJson->TryGetStringField(TEXT("transport"), Candidate.Transport);
				CandidateJson->TryGetStringField(TEXT("address"), Candidate.Address);
				Candidate.AuthorityEpoch = static_cast<uint64>(CandidateJson->GetNumberField(TEXT("authorityEpoch")));
				Candidate.Port = static_cast<int32>(CandidateJson->GetNumberField(TEXT("port")));
				Candidate.Priority = static_cast<uint32>(CandidateJson->GetNumberField(TEXT("priority")));

				if ((!ExpectedHostPeerId.IsEmpty() && Candidate.PeerId != ExpectedHostPeerId) ||
					Candidate.AuthorityEpoch < AuthorityEpoch ||
					Candidate.Address.IsEmpty() || Candidate.Port <= 0 || Candidate.Port > 65535)
				{
					continue;
				}

				if (Candidate.Transport == TEXT("Relay"))
				{
					// 记录 relay 候选:direct 不可用时的 fallback
					if (BestRelay.Address.IsEmpty() || Candidate.Priority > BestRelay.Priority)
					{
						BestRelay = MoveTemp(Candidate);
					}
					continue;
				}

				if (Candidate.Transport != TEXT("UDP"))
				{
					continue;
				}

				// 同 Epoch 下 ServerReflexive(公网)优先于内网 Host 候选。
				const int32 BestRank = RankDirectCandidate(Best);
				const int32 CandidateRank = RankDirectCandidate(Candidate);
				if (Best.Address.IsEmpty() || Candidate.AuthorityEpoch > Best.AuthorityEpoch ||
					(Candidate.AuthorityEpoch == Best.AuthorityEpoch &&
					 (CandidateRank > BestRank ||
					  (CandidateRank == BestRank && Candidate.Priority > Best.Priority))))
				{
					Best = MoveTemp(Candidate);
				}
			}

			if (!Best.Address.IsEmpty())
			{
				AuthorityEpoch = FMath::Max(AuthorityEpoch, Best.AuthorityEpoch);
				HandleHostEndpointNominated(Best);
				return;
			}

			// 无 direct 候选 → relay 回退(需已缓存 TURN 凭证,由 RelayNetDriver 拉取)
			if (!BestRelay.Address.IsEmpty() && HasRelayCredentials())
			{
				AuthorityEpoch = FMath::Max(AuthorityEpoch, BestRelay.AuthorityEpoch);
				bRelayFallbackRequested = true;
				UE_LOG(LogWeaponCentralConnectivity, Log,
					TEXT("No direct candidate; falling back to relay %s:%d"),
					*BestRelay.Address, BestRelay.Port);
				HandleHostEndpointNominated(BestRelay);
				return;
			}
			ScheduleHostCandidatePoll();
		});
	Request->ProcessRequest();
}

void UWeaponCentralConnectivitySubsystem::ScheduleHostCandidatePoll()
{
	if (!bWaitingForHostEndpoint)
	{
		return;
	}
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().SetTimer(
			HostEndpointPollTimerHandle,
			this,
			&UWeaponCentralConnectivitySubsystem::RequestHostCandidates,
			FMath::Max(0.25f, HostEndpointPollIntervalSeconds),
			false);
	}
}

void UWeaponCentralConnectivitySubsystem::StopHostEndpointWait()
{
	bWaitingForHostEndpoint = false;
	bCandidateRequestInFlight = false;
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(HostEndpointPollTimerHandle);
		GI->GetTimerManager().ClearTimer(HostEndpointWaitTimerHandle);
	}
}

void UWeaponCentralConnectivitySubsystem::HandleHostEndpointWaitTimeout()
{
	if (!bWaitingForHostEndpoint)
	{
		return;
	}
	StopHostEndpointWait();
	FailMatchAction(TEXT("Timed out waiting for the Host endpoint"));
}

FString UWeaponCentralConnectivitySubsystem::FormatEndpointAddress(
	const FString& Address,
	const int32 Port)
{
	if (Address.Contains(TEXT(":")) && !Address.StartsWith(TEXT("[")))
	{
		return FString::Printf(TEXT("[%s]:%d"), *Address, Port);
	}
	return FString::Printf(TEXT("%s:%d"), *Address, Port);
}

bool UWeaponCentralConnectivitySubsystem::TriggerDirectReconnect(const FString& Address, FString& OutError)
{
	if (!SessionSubsystem.IsValid())
	{
		OutError = TEXT("SessionSubsystem not available");
		return false;
	}

	// 校验 endpoint 地址合法性(非空 + 基本格式)
	if (Address.IsEmpty())
	{
		OutError = TEXT("Nominated endpoint has empty address");
		return false;
	}

	// 经 HostEndpointPublished 事件进入此函数前,已通过 ValidateEventFence
	// (MatchId 匹配 + Epoch 非旧)。此处再校验一次当前会话状态。
	if (!SessionSubsystem->ReconnectToMigratedHost(Address))
	{
		OutError = FString::Printf(TEXT("ReconnectToMigratedHost failed for %s"), *Address);
		return false;
	}

	UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Direct reconnect initiated to %s"), *Address);
	return true;
}

// Relay 路径:TURN 凭证获取与 NetDriver 配置
//
// Host 仍按 direct 优先发布 endpoint;relay 是 direct 不可达时的 fallback。
// 本层只负责把 Coordinator 下发的短期 TURN 凭证交给 relay NetDriver,
// 不参与候选选择与切换决策。

bool UWeaponCentralConnectivitySubsystem::RequestRelayCredentials(FString& OutError)
{
	if (!bCentralControlReady || !IsCentralMatchReady())
	{
		OutError = TEXT("Central match is not ready; cannot request relay credentials");
		return false;
	}

	FHttpModule& Http = FHttpModule::Get();
	const TSharedRef<IHttpRequest> Request = Http.CreateRequest();
	Request->SetURL(FString::Printf(
		TEXT("%s/v1/matches/%s/relay/credentials"),
		*CoordinatorBaseUrl,
		*CentralMatchIdString));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Connection"), TEXT("close"));

	// 异步:结果缓存到 RelayCredentialsCached,供 ConfigureRelayNetDriver 使用。
	Request->OnProcessRequestComplete().BindLambda(
		[this, &OutError](
			FHttpRequestPtr Req,
			const FHttpResponsePtr& Resp,
			const bool bWasSuccessful)
		{
			if (!bWasSuccessful || !Resp.IsValid() || Resp->GetResponseCode() != 200)
			{
				OutError = FString::Printf(
					TEXT("Relay credential request failed (HTTP %d)"),
					Resp.IsValid() ? Resp->GetResponseCode() : 0);
				return;
			}

			const FString Body = Resp->GetContentAsString();
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
			TSharedPtr<FJsonObject> Json;
			if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
			{
				OutError = TEXT("Invalid relay credential response");
				return;
			}

			RelayCredentialsCached = MakeShared<FRelayCredentialsCache>();
			RelayCredentialsCached->Username = Json->GetStringField(TEXT("username"));
			RelayCredentialsCached->Password = Json->GetStringField(TEXT("password"));
			RelayCredentialsCached->RelayAddr = Json->GetStringField(TEXT("relayAddr"));

			// Host:凭证到达后立即配置 Listen NetDriver 走 TURN,
			// 这样后续 PublishHostEndpoint 才能拿到 relayed 地址。
			if (CurrentMatchRole == TEXT("Host"))
			{
				FString ConfigureError;
				ConfigureRelayNetDriver(ConfigureError);
			}

			// 凭证就绪后启动 relay 事务推进(每帧 Tick),驱动 TURN Allocate 完成。
			if (UGameInstance* GI = GetGameInstance())
			{
				GI->GetTimerManager().SetTimer(
					RelayTickTimerHandle,
					this,
					&UWeaponCentralConnectivitySubsystem::TickRelayNetDriver,
					0.05f,
					true);
			}

			UE_LOG(LogWeaponCentralConnectivity, Log,
				TEXT("Relay credentials obtained for match %s (TURN %s)"),
				*CentralMatchIdString, *RelayCredentialsCached->RelayAddr);
		});
	Request->ProcessRequest();
	return true;
}

bool UWeaponCentralConnectivitySubsystem::ConfigureRelayNetDriver(FString& OutError)
{
	UWorld* World = GetWorld();
	if (!World)
	{
		OutError = TEXT("No world available");
		return false;
	}

	UNetDriver* NetDriver = World->GetNetDriver();
	if (!NetDriver)
	{
		OutError = TEXT("No active NetDriver");
		return false;
	}

	UWeaponRelayNetDriver* RelayDriver = Cast<UWeaponRelayNetDriver>(NetDriver);
	if (!RelayDriver)
	{
		OutError = FString::Printf(
			TEXT("Active NetDriver is %s, not WeaponRelayNetDriver"),
			*NetDriver->GetClass()->GetName());
		return false;
	}

	if (!RelayCredentialsCached.IsValid())
	{
		OutError = TEXT("Relay credentials are not cached; call RequestRelayCredentials first");
		return false;
	}

	RelayDriver->SetRelayConnectionInfo(
		RelayCredentialsCached->RelayAddr,
		RelayCredentialsCached->Username,
		RelayCredentialsCached->Password);

	// Host:若 Listen socket 在凭证到达前已创建(未走 TURN),热升级为
	// relay socket(RecreateSocket 仅客户端可用,Host 用 UpgradeSocketToRelay)。
	if (World->GetNetMode() == NM_ListenServer)
	{
		RelayDriver->UpgradeSocketToRelay();
	}

	UE_LOG(LogWeaponCentralConnectivity, Log,
		TEXT("Relay NetDriver configured with TURN server %s"), *RelayCredentialsCached->RelayAddr);
	return true;
}

// 向现有系统交付事实

void UWeaponCentralConnectivitySubsystem::PublishBackendReadiness()
{
	if (!HostMigration.IsValid())
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning,
			TEXT("PublishBackendReadiness skipped: HostMigration subsystem not available"));
		return;
	}

	const bool bReady = bCentralControlReady && bCentralLeaseReady && bCentralCheckpointReady;

	FWeaponMigrationBackendReadiness Readiness;
	Readiness.bCentralControlReady = bCentralControlReady;
	Readiness.bCentralLeaseReady = bCentralLeaseReady;
	Readiness.bCentralCheckpointReady = bCentralCheckpointReady;
	Readiness.bLocalTransportReady = true; // 目前先复用 IpNetDriver,本地传输始终就绪

	FString Error;
	if (!HostMigration->ApplyMigrationBackendReadiness(Readiness, Error))
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning,
			TEXT("Central backend readiness rejected: %s"), *Error);
		return;
	}

	UE_LOG(LogWeaponCentralConnectivity, Log,
		TEXT("Central backend committed: %s"),
		bReady ? TEXT("CentralAssisted") : TEXT("LocalP2P"));
}

// HTTP 设备身份管线(challenge → register → session)

namespace
{
	FString SanitizeDeviceKeySuffix(const FString& RawSuffix)
	{
		FString Result;
		for (const TCHAR Character : RawSuffix)
		{
			if (FChar::IsAlnum(Character) || Character == TEXT('-') || Character == TEXT('_'))
			{
				Result.AppendChar(Character);
			}
		}
		return Result;
	}

	FString GetDeviceKeyPath(const UWorld* World)
	{
		FString Suffix;
		FParse::Value(FCommandLine::Get(), TEXT("WeaponCentralDeviceKeySuffix="), Suffix);
#if WITH_EDITOR
		if (Suffix.IsEmpty() && World && World->GetOutermost())
		{
			const int32 PIEInstanceId = World->GetOutermost()->GetPIEInstanceID();
			if (PIEInstanceId != INDEX_NONE)
			{
				Suffix = FString::Printf(TEXT("PIE-%d"), PIEInstanceId);
			}
		}
#endif
		Suffix = SanitizeDeviceKeySuffix(Suffix);
		const FString FileName = Suffix.IsEmpty()
			? TEXT("DeviceKey.bin")
			: FString::Printf(TEXT("DeviceKey-%s.bin"), *Suffix);
		return FPaths::Combine(FPaths::ProjectSavedDir(), FileName);
	}

	bool LoadDeviceKeyPair(
		const UWorld* World,
		TArray<uint8>& OutPrivateKey,
		TArray<uint8>& OutPublicKey)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		const FString Path = GetDeviceKeyPath(World);
		if (!PlatformFile.FileExists(*Path))
		{
			return false;
		}

		const TUniquePtr<IFileHandle> File(PlatformFile.OpenRead(*Path));
		if (!File.IsValid())
		{
			return false;
		}

		const int64 FileSize = File->Size();
		// 私钥 32 + 公钥 32 = 64 字节;文件大小不对则丢弃
		if (FileSize != 64)
		{
			UE_LOG(LogWeaponCentralConnectivity, Warning,
				TEXT("Device key file has unexpected size %lld; regenerating"), FileSize);
			return false;
		}

		TArray<uint8> Bytes;
		Bytes.SetNumUninitialized(64);
		if (!File->Read(Bytes.GetData(), 64))
		{
			return false;
		}

		OutPrivateKey.SetNumUninitialized(32);
		FMemory::Memcpy(OutPrivateKey.GetData(), Bytes.GetData(), 32);
		OutPublicKey.SetNumUninitialized(32);
		FMemory::Memcpy(OutPublicKey.GetData(), Bytes.GetData() + 32, 32);
		return true;
	}

	bool SaveDeviceKeyPair(
		const UWorld* World,
		const TArray<uint8>& PrivateKey,
		const TArray<uint8>& PublicKey)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		const FString Path = GetDeviceKeyPath(World);

		// bAppend=false:覆盖写入(不是追加);bAllowRead=true
		TUniquePtr<IFileHandle> File(PlatformFile.OpenWrite(*Path, /*bAppend=*/false, /*bAllowRead=*/true));
		if (!File.IsValid())
		{
			return false;
		}

		const bool bSaved = File->Write(PrivateKey.GetData(), PrivateKey.Num()) &&
			File->Write(PublicKey.GetData(), PublicKey.Num());
		File->Flush();
		return bSaved;
	}
}

void UWeaponCentralConnectivitySubsystem::RunAuthPipeline()
{
	if (AuthState == EWeaponCentralAuthState::Authenticated)
	{
		// 内存缓存 token 仍有效 -> 跳过重认证
		UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Auth: token cached, skipping re-auth"));
		OpenEventChannel();
		return;
	}

	// 加载已持久化的设备密钥对(重启复用);不存在才生成并保存
	if (DevicePrivateKey.Num() == 0 || DevicePublicKey.Num() == 0)
	{
		if (LoadDeviceKeyPair(GetWorld(), DevicePrivateKey, DevicePublicKey))
		{
			UE_LOG(
				LogWeaponCentralConnectivity,
				Log,
				TEXT("Device key pair loaded from disk: %s"),
				*GetDeviceKeyPath(GetWorld()));
		}
		else
		{
			if (!FWeaponCentralDeviceCrypto::GenerateKeyPair(DevicePrivateKey, DevicePublicKey))
			{
				FailAuthPipeline(TEXT("Failed to generate device key pair"));
				return;
			}
			if (!SaveDeviceKeyPair(GetWorld(), DevicePrivateKey, DevicePublicKey))
			{
				FailAuthPipeline(TEXT("Failed to persist device key pair"));
				return;
			}
			UE_LOG(
				LogWeaponCentralConnectivity,
				Log,
				TEXT("Device key pair generated and persisted: %s"),
				*GetDeviceKeyPath(GetWorld()));
		}
	}

	AuthState = EWeaponCentralAuthState::ChallengeRequested;
	SendChallengeRequest();
}

void UWeaponCentralConnectivitySubsystem::SendChallengeRequest()
{
	FHttpModule& Http = FHttpModule::Get();
	const TSharedRef<IHttpRequest> Request = Http.CreateRequest();
	Request->SetURL(CoordinatorBaseUrl + TEXT("/v1/devices/challenge"));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Connection"), TEXT("close"));

	const FString PublicKeyB64 = FWeaponCentralDeviceCrypto::EncodePublicKeyBase64Url(DevicePublicKey);
	const FString Body = FString::Printf(TEXT("{\"publicKey\":\"%s\"}"), *PublicKeyB64);
	Request->SetContentAsString(Body);

	Request->OnProcessRequestComplete().BindLambda([this](FHttpRequestPtr Req, const FHttpResponsePtr& Resp, bool bWasSuccessful)
	{
		if (!bWasSuccessful || !Resp.IsValid())
		{
			FailAuthPipeline(TEXT("Challenge request failed"));
			return;
		}
		if (Resp->GetResponseCode() != 200)
		{
			FailAuthPipeline(FString::Printf(TEXT("Challenge failed (HTTP %d): %s"),
				Resp->GetResponseCode(), *Resp->GetContentAsString()));
			return;
		}

		const FString ResponseStr = Resp->GetContentAsString();
		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseStr);
		if (!FJsonSerializer::Deserialize(Reader, Json))
		{
			FailAuthPipeline(TEXT("Challenge response parse failed"));
			return;
		}

		const FString Nonce = Json->GetStringField(TEXT("nonce"));
		if (Nonce.IsEmpty())
		{
			FailAuthPipeline(TEXT("Challenge response missing nonce"));
			return;
		}

		PendingChallengeNonce = Nonce;
		AuthState = EWeaponCentralAuthState::ChallengeSigned;

		// 先确保设备已注册(Go 侧幂等:已注册返回既有 deviceId)。
		// 之后再签名 nonce 发 session。
		SendRegisterRequest(Nonce);
	});

	Request->ProcessRequest();
}

void UWeaponCentralConnectivitySubsystem::SendRegisterRequest(const FString& Nonce)
{
	const FString PublicKeyB64 = FWeaponCentralDeviceCrypto::EncodePublicKeyBase64Url(DevicePublicKey);
	const FString Body = FString::Printf(TEXT("{\"publicKey\":\"%s\"}"), *PublicKeyB64);

	FHttpModule& Http = FHttpModule::Get();
	const TSharedRef<IHttpRequest> Request = Http.CreateRequest();
	Request->SetURL(CoordinatorBaseUrl + TEXT("/v1/devices/register"));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Connection"), TEXT("close"));
	Request->SetContentAsString(Body);

	Request->OnProcessRequestComplete().BindLambda([this, Nonce](FHttpRequestPtr Req, const FHttpResponsePtr& Resp, bool bWasSuccessful)
	{
		if (!bWasSuccessful || !Resp.IsValid())
		{
			FailAuthPipeline(TEXT("Register request failed"));
			return;
		}

		const int32 StatusCode = Resp->GetResponseCode();
		// 200(已存在) / 201(新注册)都是成功;其他(如 400 / 409 非幂等路径)失败
		if (StatusCode != 200 && StatusCode != 201)
		{
			FailAuthPipeline(FString::Printf(TEXT("Register failed (HTTP %d)"), StatusCode));
			return;
		}

		// 记录 deviceId(Go 侧返回);注册成功 → 继续签名 nonce → session
		const FString ResponseStr = Resp->GetContentAsString();
		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseStr);
		if (FJsonSerializer::Deserialize(Reader, Json))
		{
			DeviceId = Json->GetStringField(TEXT("deviceId"));
		}

		// 签名 nonce(本地 CPU 操作)→ 发送 session 请求
		TArray<uint8> NonceBytes;
		FTCHARToUTF8 Converter(*Nonce);
		NonceBytes.Append(reinterpret_cast<const uint8*>(Converter.Get()), Converter.Length());
		TArray<uint8> Signature;
		if (!FWeaponCentralDeviceCrypto::Sign(DevicePrivateKey, NonceBytes, Signature))
		{
			FailAuthPipeline(TEXT("Failed to sign challenge nonce"));
			return;
		}

		// 先广播 nonce 到达(UI 进度),再发 session
		OnConnectivityChanged.Broadcast(ConnectivityState, TEXT("Nonce received, signing"));
		SendSessionRequest(Nonce, Signature);
	});

	Request->ProcessRequest();
}

void UWeaponCentralConnectivitySubsystem::SendSessionRequest(const FString& Nonce, const TArray<uint8>& Signature)
{
	// 签名也走 base64url 无 padding(Go 侧 RawURLEncoding)
	const FString SignatureB64 = FWeaponCentralDeviceCrypto::EncodeBase64UrlNoPad(Signature);
	const FString Body = FString::Printf(
		TEXT("{\"nonce\":\"%s\",\"signature\":\"%s\"}"),
		*Nonce, *SignatureB64);

	FHttpModule& Http = FHttpModule::Get();
	TSharedRef<IHttpRequest> Request = Http.CreateRequest();
	Request->SetURL(CoordinatorBaseUrl + TEXT("/v1/devices/session"));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetHeader(TEXT("Connection"), TEXT("close"));
	Request->SetContentAsString(Body);

	Request->OnProcessRequestComplete().BindLambda([this](FHttpRequestPtr Req, const FHttpResponsePtr& Resp, bool bWasSuccessful)
	{
		if (!bWasSuccessful || !Resp.IsValid())
		{
			FailAuthPipeline(TEXT("Session request failed"));
			return;
		}
		if (Resp->GetResponseCode() != 200)
		{
			FailAuthPipeline(FString::Printf(TEXT("Session failed (HTTP %d): %s"),
				Resp->GetResponseCode(), *Resp->GetContentAsString()));
			return;
		}

		const FString ResponseStr = Resp->GetContentAsString();
		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(ResponseStr);
		if (!FJsonSerializer::Deserialize(Reader, Json))
		{
			FailAuthPipeline(TEXT("Session response parse failed"));
			return;
		}

		const FString Token = Json->GetStringField(TEXT("token"));
		if (Token.IsEmpty())
		{
			FailAuthPipeline(TEXT("Session response missing token"));
			return;
		}

		HandleSessionSuccess(Token);
	});

	Request->ProcessRequest();
}

void UWeaponCentralConnectivitySubsystem::HandleSessionSuccess(const FString& Token)
{
	AccessToken = Token;
	AuthState = EWeaponCentralAuthState::Authenticated;
	ConnectivityState = EWeaponCentralConnectivityState::Connected;
	bCentralControlReady = true;

	UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Central control ready (token acquired)"));
	OnConnectivityChanged.Broadcast(ConnectivityState, TEXT("Central control ready"));

	// 把"控制连接就绪"事实交给状态机
	PublishBackendReadiness();

	// 注意:此处不打开事件通道——MatchId 尚未建立(认证期间的 Match 动作
	// 只是排队),OpenEventChannel 依赖 CentralMatchIdString,此刻调用
	// 必然打 "no active MatchId" 警告。Match 建立后(HandleMatchCreated/
	// HandleMatchJoined)会以正确的 MatchId 打开。

	// 执行认证期间排队的 Match 动作(创建/加入)
	ExecutePendingMatchAction();
}

void UWeaponCentralConnectivitySubsystem::FailAuthPipeline(const FString& Message)
{
	AuthState = EWeaponCentralAuthState::Failed;
	ConnectivityState = EWeaponCentralConnectivityState::Failed;
	UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("Auth pipeline failed: %s"), *Message);
	OnConnectivityChanged.Broadcast(ConnectivityState, Message);
}

// WebSocket 事件通道 + Epoch fencing + 指数退避重连

void UWeaponCentralConnectivitySubsystem::OpenEventChannel()
{
	// 防重入只对同一个 Match 生效；跨 Match 必须关闭旧订阅。
	if (EventSocket.IsValid() && EventSocketMatchIdString == CentralMatchIdString)
	{
		return;
	}
	if (EventSocket.IsValid())
	{
		ResetEventSocket();
	}

	// 从 CoordinatorBaseUrl 推导 WS URL:https → wss,http → ws
	FString WsUrl = CoordinatorBaseUrl;
	if (WsUrl.StartsWith(TEXT("https://")))
	{
		WsUrl = TEXT("wss://") + WsUrl.RightChop(8);
	}
	else if (WsUrl.StartsWith(TEXT("http://")))
	{
		WsUrl = TEXT("ws://") + WsUrl.RightChop(7);
	}

	// 事件通道需要 MatchId(Go 侧路由 GET /v1/matches/{matchId}/events)。
	// 无 Match 时不连。URL 用原始 matchId 字符串(Go uuid 格式),
	// 不能经 FGuid::ToString()(会变大写无连字符,Go 查库 404)。
	if (CentralMatchIdString.IsEmpty())
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("OpenEventChannel skipped: no active MatchId"));
		return;
	}
	WsUrl += FString::Printf(TEXT("/v1/matches/%s/events"), *CentralMatchIdString);
	UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Opening event channel: %s"), *WsUrl);

	FWebSocketsModule& WsModule = FModuleManager::GetModuleChecked<FWebSocketsModule>(TEXT("WebSockets"));
	// WS 握手 = HTTP 升级,必须带 Bearer token(Go 侧 events 路由 RequireDevice)
	TMap<FString, FString> UpgradeHeaders;
	if (!AccessToken.IsEmpty())
	{
		UpgradeHeaders.Add(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));
	}
	EventSocket = WsModule.CreateWebSocket(WsUrl, TEXT(""), UpgradeHeaders);
	EventSocketMatchIdString = CentralMatchIdString;
	const FString SocketMatchId = EventSocketMatchIdString;

	EventSocket->OnConnected().AddLambda([this, SocketMatchId]()
	{
		if (EventSocketMatchIdString != SocketMatchId)
		{
			return;
		}
		ReconnectAttempt = 0;
		ConnectivityState = EWeaponCentralConnectivityState::Connected;
		UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Event channel connected"));
		OnConnectivityChanged.Broadcast(ConnectivityState, TEXT("Event channel connected"));
	});

	EventSocket->OnConnectionError().AddLambda([this, SocketMatchId](const FString& Error)
	{
		if (EventSocketMatchIdString != SocketMatchId)
		{
			return;
		}
		UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("Event channel error: %s"), *Error);
		ScheduleReconnect();
	});

	EventSocket->OnClosed().AddLambda([this, SocketMatchId](int32 StatusCode, const FString& Reason, bool bWasClean)
	{
		if (EventSocketMatchIdString != SocketMatchId)
		{
			return;
		}
		UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Event channel closed: %d %s"), StatusCode, *Reason);
		ScheduleReconnect();
	});

	EventSocket->OnMessage().AddLambda([this, SocketMatchId](const FString& Message)
	{
		if (EventSocketMatchIdString == SocketMatchId)
		{
			HandleEventMessage(Message);
		}
	});

	EventSocket->Connect();
}

void UWeaponCentralConnectivitySubsystem::ResetEventSocket()
{
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(ReconnectTimerHandle);
	}
	if (EventSocket.IsValid())
	{
		EventSocket->OnConnected().Clear();
		EventSocket->OnConnectionError().Clear();
		EventSocket->OnClosed().Clear();
		EventSocket->OnMessage().Clear();
		EventSocket->Close();
		EventSocket.Reset();
	}
	EventSocketMatchIdString.Reset();
}

void UWeaponCentralConnectivitySubsystem::HandleEventMessage(const FString& Message)
{
	TSharedPtr<FJsonObject> Json;
	const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Message);
	if (!FJsonSerializer::Deserialize(Reader, Json))
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("Event parse failed: %s"), *Message);
		return;
	}

	const FString EventType = Json->GetStringField(TEXT("type"));
	const FGuid EventMatchId(Json->GetStringField(TEXT("matchId")));
	const uint64 EventEpoch = static_cast<uint64>(Json->GetNumberField(TEXT("epoch")));

	// 本地尚无 MatchId:事件暂存(只存原文),MatchId 建立后重放时再校验
	if (!MatchId.IsValid())
	{
		HandleEventStashed(Message);
		return;
	}

	HandleEventDispatch(Json, EventType, EventMatchId, EventEpoch);
}

void UWeaponCentralConnectivitySubsystem::HandleEventStashed(const FString& RawEvent)
{
	// 只存原文,不存判断结论;ReplayStashedEvents 重放时走完整 fence
	UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Event stashed (no MatchId yet)"));
	StashedEvents.Add(RawEvent);
}

void UWeaponCentralConnectivitySubsystem::HandleEventDispatch(
	const TSharedPtr<FJsonObject>& Json,
	const FString& EventType,
	const FGuid& EventMatchId,
	const uint64 EventEpoch)
{
	// Epoch fencing:任何缺少 Match/Epoch fencing 的事件都被拒绝
	if (!ValidateEventFence(EventMatchId, EventEpoch))
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning,
			TEXT("Event rejected (fence): type=%s match=%s epoch=%llu"),
			*EventType, *EventMatchId.ToString(), EventEpoch);
		return;
	}

	// 事件顶层 peerId(来自 Go 侧 Event.PeerId,token 的 sub)
	const FString EventPeerId = Json->GetStringField(TEXT("peerId"));

	if (EventType == TEXT("LeaseGranted"))
	{
		// 记录当前 Lease 的 Host 身份,供后续 HostEndpointPublished 校验
		ExpectedHostPeerId = EventPeerId;

		FWeaponCentralHostLease Lease;
		Lease.MatchId = EventMatchId;
		Lease.AuthorityEpoch = EventEpoch;
		HandleLeaseGranted(Lease);
	}
	else if (EventType == TEXT("LeaseRenewed"))
	{
		if (ExpectedHostPeerId.IsEmpty() || EventPeerId == ExpectedHostPeerId)
		{
			AuthorityEpoch = EventEpoch;
		}
	}
	else if (EventType == TEXT("HostEndpointPublished"))
	{
		// peerId 校验:发布 endpoint 的必须与当前 Lease 的 Host 一致
		if (!ExpectedHostPeerId.IsEmpty() && EventPeerId != ExpectedHostPeerId)
		{
			UE_LOG(LogWeaponCentralConnectivity, Warning,
				TEXT("HostEndpointPublished rejected: peerId %s != expected host %s"),
				*EventPeerId, *ExpectedHostPeerId);
			return;
		}

		FWeaponCentralEndpointCandidate Endpoint;
		Endpoint.MatchId = EventMatchId.ToString();
		Endpoint.AuthorityEpoch = EventEpoch;
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		if (Json->TryGetObjectField(TEXT("payload"), Payload) && Payload->IsValid())
		{
			Endpoint.Address = (*Payload)->GetStringField(TEXT("address"));
			Endpoint.Port = (*Payload)->GetIntegerField(TEXT("port"));
		}

		// 不直接 travel:事件只携带 Host 提名的单一地址(可能是内网地址)。
		// 重新拉候选列表,由 RequestHostCandidates 按可达性(ServerReflexive
		// > Host)挑选,避免远程玩家被带往不可达的内网地址。
		if (bWaitingForHostEndpoint)
		{
			RequestHostCandidates();
		}
	}
	else if (EventType == TEXT("CandidateUpdated"))
	{
		if (bWaitingForHostEndpoint &&
			(ExpectedHostPeerId.IsEmpty() || EventPeerId == ExpectedHostPeerId))
		{
			RequestHostCandidates();
		}
	}
	else if (EventType == TEXT("CheckpointCommitted"))
	{
		const TSharedPtr<FJsonObject>* Payload = nullptr;
		int64 Revision = 0;
		if (Json->TryGetObjectField(TEXT("payload"), Payload) && Payload->IsValid())
		{
			Revision = (*Payload)->GetIntegerField(TEXT("snapshotRevision"));
		}
		HandleCheckpointCommitted(EventMatchId, EventEpoch, Revision);
	}
	else
	{
		UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Event ignored (unhandled type): %s"), *EventType);
	}
}

void UWeaponCentralConnectivitySubsystem::ReplayStashedEvents()
{
	if (StashedEvents.IsEmpty())
	{
		return;
	}

	UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Replaying %d stashed events"), StashedEvents.Num());
	TArray<FString> ToReplay = MoveTemp(StashedEvents);
	for (const FString& RawEvent : ToReplay)
	{
		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(RawEvent);
		if (!FJsonSerializer::Deserialize(Reader, Json))
		{
			continue;
		}
		const FString EventType = Json->GetStringField(TEXT("type"));
		const FGuid EventMatchId(Json->GetStringField(TEXT("matchId")));
		const uint64 EventEpoch = static_cast<uint64>(Json->GetNumberField(TEXT("epoch")));
		HandleEventDispatch(Json, EventType, EventMatchId, EventEpoch);
	}
}

bool UWeaponCentralConnectivitySubsystem::ValidateEventFence(const FGuid& EventMatchId, uint64 EventEpoch)
{
	// MatchId 必须匹配本地战局
	if (!MatchId.IsValid())
	{
		// 本地尚未进入 Match;事件可暂存或拒绝。这里拒绝(保守)。
		return false;
	}
	if (EventMatchId != MatchId)
	{
		return false;
	}

	// 旧 Epoch 拒绝;新 Epoch 接受并更新
	if (EventEpoch < AuthorityEpoch)
	{
		return false;
	}
	if (EventEpoch > AuthorityEpoch)
	{
		AuthorityEpoch = EventEpoch;
	}
	return true;
}

void UWeaponCentralConnectivitySubsystem::TickRelayNetDriver()
{
	UWorld* World = GetWorld();
	UNetDriver* NetDriver = World ? World->GetNetDriver() : nullptr;
	if (UWeaponRelayNetDriver* RelayDriver = Cast<UWeaponRelayNetDriver>(NetDriver))
	{
		RelayDriver->TickRelay();
	}
}

void UWeaponCentralConnectivitySubsystem::HandleLeaseGranted(const FWeaponCentralHostLease& Lease)
{
	// Lease 已通过 fencing;置第二门锁并报告事实
	AuthorityEpoch = Lease.AuthorityEpoch;
	bCentralLeaseReady = true;
	UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Central lease ready (epoch %llu)"), Lease.AuthorityEpoch);
	PublishBackendReadiness();
}

void UWeaponCentralConnectivitySubsystem::HandleHostEndpointNominated(const FWeaponCentralEndpointCandidate& Endpoint)
{
	if (!bWaitingForHostEndpoint)
	{
		return;
	}
	if (Endpoint.Port <= 0 || Endpoint.Port > 65535)
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("Host endpoint rejected: invalid port %d"), Endpoint.Port);
		return;
	}

	// 已通过 fencing 的 direct endpoint → 触发 reconnect
	const FString DirectAddress = FormatEndpointAddress(Endpoint.Address, Endpoint.Port);
	FString Error;
	if (!TriggerDirectReconnect(DirectAddress, Error))
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("Reconnect failed: %s"), *Error);
		StopHostEndpointWait();
		FailMatchAction(Error);
		return;
	}
	StopHostEndpointWait();
	ConnectivityState = EWeaponCentralConnectivityState::DirectConnected;
	OnConnectivityChanged.Broadcast(ConnectivityState, TEXT("Direct endpoint connected"));
}

int32 UWeaponCentralConnectivitySubsystem::RankDirectCandidate(const FWeaponCentralEndpointCandidate& Candidate)
{
	// 公网可达性排序:ServerReflexive > Host(内网/手动)。Relay 走单独路径。
	if (Candidate.CandidateType == TEXT("ServerReflexive"))
	{
		return 2;
	}
	if (Candidate.CandidateType == TEXT("Host"))
	{
		return 1;
	}
	return 0;
}

void UWeaponCentralConnectivitySubsystem::HandleCheckpointCommitted(const FGuid& InMatchId, uint64 Epoch, int64 Revision)
{
	// 已通过 fencing;置第三门锁并报告事实
	bCentralCheckpointReady = true;
	UE_LOG(LogWeaponCentralConnectivity, Log,
		TEXT("Central checkpoint ready (epoch %llu revision %lld)"), Epoch, Revision);
	PublishBackendReadiness();
}

void UWeaponCentralConnectivitySubsystem::ScheduleReconnect()
{
	// 指数退避 + 超时放弃
	ResetEventSocket();

	if (ReconnectAttempt == 0)
	{
		ReconnectDeadlineSeconds = FPlatformTime::Seconds() + ReconnectTimeoutSeconds;
	}

	if (FPlatformTime::Seconds() > ReconnectDeadlineSeconds)
	{
		// 超时放弃:控制面不可用,但不切换 backend(锁在 Match 内不松)
		bCentralControlReady = false;
		ConnectivityState = EWeaponCentralConnectivityState::Failed;
		UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("Event reconnect timed out; central control unavailable"));
		OnConnectivityChanged.Broadcast(ConnectivityState, TEXT("Central unreachable; backend latch unchanged"));
		return;
	}

	const float Delay = FMath::Min(
		ReconnectBaseDelaySeconds * FMath::Pow(2.0f, ReconnectAttempt),
		ReconnectMaxDelaySeconds);
	ReconnectAttempt++;

	// 用 GameInstance 的 TimerManager 调度重连
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().SetTimer(ReconnectTimerHandle, [this]()
		{
			OpenEventChannel();
		}, Delay, false);
	}
}

void UWeaponCentralConnectivitySubsystem::HandleReconnectTimeout()
{
	// 由 ScheduleReconnect 内联处理,此函数保留为显式超时入口
}
