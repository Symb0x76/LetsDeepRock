// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponCentralConnectivitySubsystem.h"

#include "WeaponCandidateProber.h"
#include "WeaponCentralDeviceCrypto.h"
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
#include "Engine/Engine.h"
#include "Engine/EngineBaseTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Misc/CommandLine.h"
#include "Misc/CoreDelegates.h"
#include "Misc/Parse.h"
#include "Modules/ModuleManager.h"
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

	// 连接失败感知:与 HostMigration 各自挂全局回调,按各自状态过滤互不冲突。
	// 候选试探需要 NetworkFailure(engine 在连接超时时广播)与 travel 完成信号。
	if (GEngine)
	{
		NetworkFailureDelegateHandle = GEngine->OnNetworkFailure().AddUObject(
			this,
			&UWeaponCentralConnectivitySubsystem::HandleNetworkFailure);
	}
	PostLoadMapDelegateHandle = FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(
		this,
		&UWeaponCentralConnectivitySubsystem::HandlePostLoadMap);

	ConnectivityState = EWeaponCentralConnectivityState::Disconnected;
	UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Central Connectivity subsystem initialized"));
}

void UWeaponCentralConnectivitySubsystem::Deinitialize()
{
	if (SessionSubsystem.IsValid())
	{
		SessionSubsystem->OnLanHostComplete.RemoveAll(this);
	}
	if (GEngine && NetworkFailureDelegateHandle.IsValid())
	{
		GEngine->OnNetworkFailure().Remove(NetworkFailureDelegateHandle);
		NetworkFailureDelegateHandle.Reset();
	}
	if (PostLoadMapDelegateHandle.IsValid())
	{
		FCoreUObjectDelegates::PostLoadMapWithWorld.Remove(PostLoadMapDelegateHandle);
		PostLoadMapDelegateHandle.Reset();
	}
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(ReconnectTimerHandle);
		GI->GetTimerManager().ClearTimer(LeaseHeartbeatTimerHandle);
		GI->GetTimerManager().ClearTimer(HostEndpointPollTimerHandle);
		GI->GetTimerManager().ClearTimer(HostEndpointWaitTimerHandle);
		GI->GetTimerManager().ClearTimer(RelayTickTimerHandle);
		GI->GetTimerManager().ClearTimer(CandidateProbeTimeoutTimerHandle);
		GI->GetTimerManager().ClearTimer(ReflexivePublishTimerHandle);
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

	// 先复位旧 Match 传输状态(含 SnapshotCoordinator 的 ActiveMatchId,
	// 否则上一局残留会导致 AdoptActiveMatchId 失败),再 adopt 新 MatchId。
	ResetMatchTransportState();

	FString AdoptError;
	if (!SnapshotCoordinator->AdoptActiveMatchId(InMatchId, AdoptError))
	{
		FailMatchAction(FString::Printf(TEXT("Cannot adopt central MatchId: %s"), *AdoptError));
		return;
	}

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

	// 先复位旧 Match 传输状态,再 adopt 新 MatchId(顺序不可颠倒:
	// ResetMatchTransportState 会 ClearActiveMatchId,若在 adopt 后执行会把
	// 刚建立的 MatchId 清掉,导致后续快照/迁移找不到有效战局身份)。
	ResetMatchTransportState();

	FString AdoptError;
	if (!SnapshotCoordinator->AdoptActiveMatchId(InMatchId, AdoptError))
	{
		FailMatchAction(FString::Printf(TEXT("Cannot adopt central MatchId: %s"), *AdoptError));
		return;
	}

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
		GI->GetTimerManager().ClearTimer(CandidateProbeTimeoutTimerHandle);
		GI->GetTimerManager().ClearTimer(ReflexivePublishTimerHandle);
		GI->GetTimerManager().ClearTimer(RelayTickTimerHandle);
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
	// 复位候选试探状态:换一局重新加入时,残留的 relay 标记会跳过 direct。
	bRelayFallbackRequested = false;
	bCandidateProbeActive = false;
	bRelayProbeInProgress = false;
	CandidateQueue.Reset();
	CandidateProbeIndex = INDEX_NONE;
	PendingCandidateProbeAddress.Reset();
	// Host 侧 STUN 反射缓存:新局重新查询,避免发布旧局地址。
	CachedReflexiveAddress.Reset();
	CachedReflexivePort = 0;
	bStunQueryCompleted = false;

	// 复位快照协调器的逻辑战局身份:换房间/重新加入时,旧 MatchId 残留会
	// 导致新一局 AdoptActiveMatchId 失败("A different logical MatchId is already active")。
	if (SnapshotCoordinator.IsValid())
	{
		SnapshotCoordinator->ClearActiveMatchId();
	}
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

	// ICE 全量发布:LAN / relayed / STUN 反射三类候选互不 return,全部上传。
	// 客户端拿到完整列表后按优先级逐个试探,LAN 用户直连,跨 NAT 用户回退 relay。

	// ① 内网/手动配置的 direct 候选(始终发布;同 NAT 客户端可直接连)。
	UploadHostCandidate(TEXT("Host"), Address, Port, 100, TEXT("listen"));
	NominateHostEndpoint(Address, Port);

	// ② relayed 候选:就绪即上传(幂等 upsert)。地址未就绪(Allocate 未完成)
	//    时为空操作,下一次 heartbeat 会自动补发,不影响 direct 候选。
	PublishRelayedHostCandidate();

	// ③ STUN 反射候选:只查询一次(阻塞 3s),延迟到下一帧避免卡住 Lease 回调;
	//    结果缓存后每次 heartbeat 复用。查询失败不影响前两类候选。
	if (!bStunQueryEnabled)
	{
		return;
	}
	if (bStunQueryCompleted)
	{
		if (!CachedReflexiveAddress.IsEmpty() && CachedReflexivePort > 0)
		{
			// 反射地址只提供公网 IP;端口用游戏监听端口(与现逻辑一致)。
			UploadHostCandidate(TEXT("ServerReflexive"), CachedReflexiveAddress, Port, 90, TEXT("stun"));
			NominateHostEndpoint(CachedReflexiveAddress, Port);
			UE_LOG(LogWeaponCentralConnectivity, Log,
				TEXT("Published server-reflexive candidate %s:%d (cached)"),
				*CachedReflexiveAddress, Port);
		}
		return;
	}

	ScheduleReflexivePublish();
}

void UWeaponCentralConnectivitySubsystem::ScheduleReflexivePublish()
{
	if (ReflexivePublishTimerHandle.IsValid())
	{
		return;
	}
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().SetTimer(
			ReflexivePublishTimerHandle,
			this,
			&UWeaponCentralConnectivitySubsystem::QueryAndPublishReflexive,
			FMath::Max(0.05f, ReflexivePublishDelaySeconds),
			false);
	}
}

void UWeaponCentralConnectivitySubsystem::QueryAndPublishReflexive()
{
	ReflexivePublishTimerHandle.Invalidate();
	bStunQueryCompleted = true;

	QueryStunReflexiveAddress(CachedReflexiveAddress, CachedReflexivePort);
	if (CachedReflexiveAddress.IsEmpty() || CachedReflexivePort <= 0)
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning,
			TEXT("STUN reflexive lookup failed; keeping LAN-only candidates"));
		return;
	}

	FString Address;
	int32 Port = 0;
	FString Error;
	if (!ResolveHostEndpoint(Address, Port, Error))
	{
		return;
	}
	// 反射端口是临时查询 socket 的随机 NAT 映射,与游戏监听端口无关;
	// 候选与提名都用游戏端口,反射地址仅提供公网 IP 部分。
	UploadHostCandidate(TEXT("ServerReflexive"), CachedReflexiveAddress, Port, 90, TEXT("stun"));
	NominateHostEndpoint(CachedReflexiveAddress, Port);
	UE_LOG(LogWeaponCentralConnectivity, Log,
		TEXT("Published server-reflexive candidate %s:%d (stun port %d)"),
		*CachedReflexiveAddress, Port, CachedReflexivePort);
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
	// 候选试探进行中:不再拉取候选(CandidateUpdated 事件触发的重拉会被忽略)。
	if (bCandidateProbeActive || !bWaitingForHostEndpoint || bCandidateRequestInFlight ||
		AccessToken.IsEmpty() || CentralMatchIdString.IsEmpty() || DeviceId.IsEmpty())
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

			// 响应返回时可能已进入试探(请求发出时尚未置位);不重启。
			if (bCandidateProbeActive)
			{
				return;
			}

			BuildCandidateQueue(*Candidates);
			if (CandidateQueue.Num() > 0)
			{
				BeginCandidateProbe();
				return;
			}
			ScheduleHostCandidatePoll();
		});
	Request->ProcessRequest();
}

void UWeaponCentralConnectivitySubsystem::BuildCandidateQueue(
	const TArray<TSharedPtr<FJsonValue>>& Candidates)
{
	CandidateQueue.Reset();

	TArray<FWeaponCentralEndpointCandidate> DirectCandidates;
	for (const TSharedPtr<FJsonValue>& Value : Candidates)
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
			// relay 候选无条件入队尾。TURN 凭证是 travel 时才需要(ProbeNextCandidate
			// 里校验),不应在此过滤——否则凭证异步晚到时 relay 候选被丢弃,队列只剩
			// direct,导致跨 NAT 用户永远无法回退 relay。
			CandidateQueue.Add(MoveTemp(Candidate));
			continue;
		}

		if (Candidate.Transport != TEXT("UDP"))
		{
			continue;
		}
		DirectCandidates.Add(MoveTemp(Candidate));
	}

	// direct 候选排序:Epoch 降序 → 可达性 rank 降序 → priority 降序。
	// ServerReflexive(公网)排在内网 Host 之前;Epoch 新者优先。
	DirectCandidates.Sort(
		[](const FWeaponCentralEndpointCandidate& A, const FWeaponCentralEndpointCandidate& B)
		{
			if (A.AuthorityEpoch != B.AuthorityEpoch)
			{
				return A.AuthorityEpoch > B.AuthorityEpoch;
			}
			const int32 RankA = RankDirectCandidate(A);
			const int32 RankB = RankDirectCandidate(B);
			if (RankA != RankB)
			{
				return RankA > RankB;
			}
			return A.Priority > B.Priority;
		});

	const int32 NumDirect = DirectCandidates.Num();
	CandidateQueue.Insert(MoveTemp(DirectCandidates), 0);
	UE_LOG(LogWeaponCentralConnectivity, Log,
		TEXT("Candidate queue built: %d direct + %d relay"),
		NumDirect, CandidateQueue.Num() - NumDirect);
}

void UWeaponCentralConnectivitySubsystem::BeginCandidateProbe()
{
	if (bCandidateProbeActive || CandidateQueue.Num() == 0)
	{
		return;
	}

	bCandidateProbeActive = true;
	CandidateProbeIndex = 0;

	// 试探开始:停止候选轮询与 20s 等待定时器(bWaitingForHostEndpoint 保留,
	// 供 CandidateUpdated 事件守卫与 StopHostEndpointWait 收敛)。
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(HostEndpointPollTimerHandle);
		GI->GetTimerManager().ClearTimer(HostEndpointWaitTimerHandle);
	}

	// 并行测速:同时 ping 全部候选,按 RTT 决定 travel 顺序,避免逐个等 10s 超时。
	if (CandidatePingWindowSeconds > 0.0f)
	{
		CandidatePinger = MakeShared<FWeaponCandidateProber>();
		FString TurnAddr, TurnUser, TurnPass;
		const bool bHasTurn = GetCachedRelayCredentials(TurnAddr, TurnUser, TurnPass);
		for (int32 Index = 0; Index < CandidateQueue.Num(); ++Index)
		{
			const FWeaponCentralEndpointCandidate& Candidate = CandidateQueue[Index];
			if (Candidate.Transport != TEXT("Relay"))
			{
				CandidatePinger->AddDirectTarget(Index, Candidate.Address, Candidate.Port);
			}
			else if (bHasTurn)
			{
				CandidatePinger->AddRelayTarget(Index, Candidate.Address, Candidate.Port, TurnAddr, TurnUser, TurnPass);
			}
		}
		UGameInstance* GI = GetGameInstance();
		if (GI && CandidatePinger->Start(CandidatePingWindowSeconds))
		{
			GI->GetTimerManager().SetTimer(
				CandidatePingTimerHandle, this, &UWeaponCentralConnectivitySubsystem::TickCandidatePing, 0.02f, true);
			return;
		}
		CandidatePinger.Reset();
	}

	ProbeNextCandidate();
}

void UWeaponCentralConnectivitySubsystem::TickCandidatePing()
{
	if (!bCandidateProbeActive || !CandidatePinger.IsValid())
	{
		return;
	}
	if (!CandidatePinger->Tick())
	{
		return;
	}
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(CandidatePingTimerHandle);
	}
	ApplyCandidatePingResults();
	CandidatePinger.Reset(); // 释放测速 socket(含 TURN allocation)
	ProbeNextCandidate();
}

void UWeaponCentralConnectivitySubsystem::ApplyCandidatePingResults()
{
	TArray<FWeaponCandidateProber::FTarget> Answered = CandidatePinger->GetTargets().FilterByPredicate(
		[](const FWeaponCandidateProber::FTarget& Target) { return Target.RttSeconds >= 0.0; });
	if (Answered.Num() == 0)
	{
		// 无应答(如旧版 Host 不回 pong):保持原有顺序逐个 travel
		UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Candidate ping: no answers, keeping default order"));
		return;
	}

	// RTT 升序;相等时 direct 优先(不占 TURN 带宽)
	Answered.Sort([](const FWeaponCandidateProber::FTarget& A, const FWeaponCandidateProber::FTarget& B)
	{
		if (A.RttSeconds != B.RttSeconds)
		{
			return A.RttSeconds < B.RttSeconds;
		}
		return !A.bRelay && B.bRelay;
	});

	TArray<FWeaponCentralEndpointCandidate> Reordered;
	for (const FWeaponCandidateProber::FTarget& Target : Answered)
	{
		Reordered.Add(CandidateQueue[Target.QueueIndex]);
	}
	// 未应答的 relay 候选保留在队尾兜底(测速 allocation 可能慢于时间窗);未应答的 direct 丢弃
	for (int32 Index = 0; Index < CandidateQueue.Num(); ++Index)
	{
		const bool bAnswered = Answered.ContainsByPredicate(
			[Index](const FWeaponCandidateProber::FTarget& Target) { return Target.QueueIndex == Index; });
		if (!bAnswered && CandidateQueue[Index].Transport == TEXT("Relay"))
		{
			Reordered.Add(CandidateQueue[Index]);
		}
	}

	UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Candidate ping: fastest %s:%d (%s, %.1f ms), %d of %d answered"),
		*Reordered[0].Address, Reordered[0].Port, Answered[0].bRelay ? TEXT("relay") : TEXT("direct"),
		Answered[0].RttSeconds * 1000.0, Answered.Num(), CandidateQueue.Num());
	CandidateQueue = MoveTemp(Reordered);
	CandidateProbeIndex = 0;
}

void UWeaponCentralConnectivitySubsystem::ProbeNextCandidate()
{
	if (!bCandidateProbeActive || !CandidateQueue.IsValidIndex(CandidateProbeIndex))
	{
		return;
	}

	const FWeaponCentralEndpointCandidate& Candidate = CandidateQueue[CandidateProbeIndex];
	const bool bIsRelay = Candidate.Transport == TEXT("Relay");
	const float Timeout = bIsRelay ? RelayProbeTimeoutSeconds : CandidateProbeTimeoutSeconds;

	// relay 候选:必须先把 bRelayFallbackRequested 置位,新世界 NetDriver 的
	// CreateSocketForProtocol 才会走 TURN(ShouldUseRelayTransport 读该 flag)。
	// 凭证可能尚未异步到达;缺失时跳过本候选(回轮询等凭证),而非直接 travel。
	if (bIsRelay)
	{
		if (!HasRelayCredentials())
		{
			UE_LOG(LogWeaponCentralConnectivity, Log,
				TEXT("Skipping relay candidate %s: TURN credentials not yet cached"),
				*Candidate.Address);
			HandleCandidateProbeFailure();
			return;
		}
		bRelayProbeInProgress = true;
		bRelayFallbackRequested = true;
	}
	else
	{
		// 测速后 relay 可能排在 direct 之前:失败回退到 direct 时必须清掉 relay 标志,
		// 否则新 NetDriver 仍会创建 TURN socket。
		bRelayProbeInProgress = false;
		bRelayFallbackRequested = false;
	}

	// travel URL 附带该候选的短超时:engine 到时广播 ConnectionTimeout,
	// 子系统据以推进下一候选,无需改全局 CDO/ini 超时值。
	const FString ProbeUrl = FString::Printf(
		TEXT("%s?InitialConnectTimeout=%.1f?ConnectionTimeout=%.1f"),
		*FormatEndpointAddress(Candidate.Address, Candidate.Port),
		Timeout,
		Timeout);
	PendingCandidateProbeAddress = ProbeUrl;

	UE_LOG(LogWeaponCentralConnectivity, Log,
		TEXT("Probing candidate %d/%d (%s) %s timeout %.1fs"),
		CandidateProbeIndex + 1, CandidateQueue.Num(),
		bIsRelay ? TEXT("relay") : *Candidate.CandidateType,
		*ProbeUrl,
		Timeout);

	FString Error;
	if (!TriggerDirectReconnect(ProbeUrl, Error))
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning,
			TEXT("Cannot initiate probe to %s: %s"), *ProbeUrl, *Error);
		HandleCandidateProbeFailure();
		return;
	}

	// 安全网超时:engine 超时(travel URL 选项)应先行,此定时器只做兜底去重。
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().SetTimer(
			CandidateProbeTimeoutTimerHandle,
			this,
			&UWeaponCentralConnectivitySubsystem::HandleCandidateProbeTimeout,
			Timeout + 2.0f,
			false);
	}
}

void UWeaponCentralConnectivitySubsystem::HandleCandidateProbeTimeout()
{
	// 安全网:engine 未在预期时间广播失败(如 pending 阶段取消),中止挂起的
	// map change 后推进。bCandidateProbeActive 已清则说明 engine 已处理过。
	if (!bCandidateProbeActive)
	{
		return;
	}
	if (UWorld* World = GetWorld())
	{
		GEngine->CancelPendingMapChange(World);
	}
	HandleCandidateProbeFailure();
}

void UWeaponCentralConnectivitySubsystem::HandleCandidateProbeFailure()
{
	if (!bCandidateProbeActive)
	{
		return;
	}
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(CandidateProbeTimeoutTimerHandle);
	}

	CandidateProbeIndex++;
	if (CandidateQueue.IsValidIndex(CandidateProbeIndex))
	{
		ProbeNextCandidate();
		return;
	}

	// 全部 direct 失败且队列里还没有 relay 候选:Host 的 TURN Allocate 可能
	// 稍后完成、heartbeat 会补发 relayed 候选。回轮询等待,而不是终局失败。
	// 重新上膛 20s 等待定时器,保证总体超时仍生效(否则会无限轮询)。
	if (HasRelayCredentials() && !bRelayProbeInProgress)
	{
		bCandidateProbeActive = false;
		if (UGameInstance* GI = GetGameInstance())
		{
			GI->GetTimerManager().SetTimer(
				HostEndpointWaitTimerHandle,
				this,
				&UWeaponCentralConnectivitySubsystem::HandleHostEndpointWaitTimeout,
				FMath::Max(1.0f, HostEndpointWaitTimeoutSeconds),
				false);
		}
		ScheduleHostCandidatePoll();
		return;
	}

	ClearCandidateProbe(true, TEXT("All host candidates failed"));
}

void UWeaponCentralConnectivitySubsystem::HandleCandidateProbeSucceeded()
{
	if (!bCandidateProbeActive)
	{
		return;
	}
	const bool bConnectedViaRelay = bRelayProbeInProgress;
	ClearCandidateProbe(false, FString());
	StopHostEndpointWait();
	ConnectivityState = bConnectedViaRelay
		? EWeaponCentralConnectivityState::Relayed
		: EWeaponCentralConnectivityState::DirectConnected;
	OnConnectivityChanged.Broadcast(
		ConnectivityState,
		bConnectedViaRelay ? TEXT("Connected via relay") : TEXT("Direct endpoint connected"));
}

void UWeaponCentralConnectivitySubsystem::ClearCandidateProbe(bool bTerminal, const FString& Message)
{
	bCandidateProbeActive = false;
	bRelayProbeInProgress = false;
	PendingCandidateProbeAddress.Reset();
	CandidatePinger.Reset();
	if (UGameInstance* GI = GetGameInstance())
	{
		GI->GetTimerManager().ClearTimer(CandidateProbeTimeoutTimerHandle);
		GI->GetTimerManager().ClearTimer(CandidatePingTimerHandle);
	}
	if (bTerminal)
	{
		StopHostEndpointWait();
		FailMatchAction(Message);
	}
}

void UWeaponCentralConnectivitySubsystem::HandleNetworkFailure(
	UWorld* World,
	UNetDriver* NetDriver,
	const ENetworkFailure::Type FailureType,
	const FString& ErrorString)
{
	if (!bCandidateProbeActive || !NetDriver || NetDriver->GetNetMode() != NM_Client)
	{
		return;
	}
	if (World && World->GetGameInstance() != GetGameInstance())
	{
		return;
	}

	const bool bIsConnectionFailure =
		FailureType == ENetworkFailure::ConnectionTimeout ||
		FailureType == ENetworkFailure::PendingConnectionFailure ||
		FailureType == ENetworkFailure::ConnectionLost;
	if (!bIsConnectionFailure)
	{
		return;
	}

	// 地址匹配防串扰:只响应当前试探候选的失败,忽略前一候选的迟到回调。
	// ServerConnection 为空/无 host 时仍推进(无法匹配;但 pending 阶段的
	// NetDriver 通常已有 URL,空洞仅发生在异常场景)。
	const FString PendingHost = FURL(nullptr, *PendingCandidateProbeAddress, TRAVEL_Absolute).Host;
	if (NetDriver->ServerConnection && !NetDriver->ServerConnection->URL.Host.IsEmpty() &&
		!PendingHost.IsEmpty())
	{
		const FString FailureHost = NetDriver->ServerConnection->URL.Host;
		if (FailureHost != PendingHost)
		{
			UE_LOG(LogWeaponCentralConnectivity, Verbose,
				TEXT("Network failure for %s ignored (probing %s)"),
				*FailureHost, *PendingHost);
			return;
		}
	}

	UE_LOG(LogWeaponCentralConnectivity, Warning,
		TEXT("Candidate probe connection failure (%s): %s"),
		ENetworkFailure::ToString(FailureType), *ErrorString);
	HandleCandidateProbeFailure();
}

void UWeaponCentralConnectivitySubsystem::HandlePostLoadMap(UWorld* LoadedWorld)
{
	if (!bCandidateProbeActive || !LoadedWorld ||
		LoadedWorld->GetGameInstance() != GetGameInstance() ||
		LoadedWorld->GetNetMode() != NM_Client)
	{
		return;
	}

	// 目标世界必须是当前试探的 host 世界:若 travel 被引擎回退到 return/menu
	// 世界(同为 NM_Client),不能误判为探测成功。
	const FString PendingHost = FURL(nullptr, *PendingCandidateProbeAddress, TRAVEL_Absolute).Host;
	if (!PendingHost.IsEmpty() && !LoadedWorld->URL.Host.IsEmpty() &&
		LoadedWorld->URL.Host != PendingHost)
	{
		UE_LOG(LogWeaponCentralConnectivity, Verbose,
			TEXT("PostLoadMap world %s ignored (probing %s)"),
			*LoadedWorld->URL.Host, *PendingHost);
		return;
	}
	HandleCandidateProbeSucceeded();
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
	// 回调异步执行时调用方栈帧早已返回:不能捕获 OutError 引用(悬垂写),失败只记日志。
	Request->OnProcessRequestComplete().BindLambda(
		[this](
			FHttpRequestPtr Req,
			const FHttpResponsePtr& Resp,
			const bool bWasSuccessful)
		{
			if (!bWasSuccessful || !Resp.IsValid() || Resp->GetResponseCode() != 200)
			{
				UE_LOG(LogWeaponCentralConnectivity, Warning,
					TEXT("Relay credential request failed (HTTP %d)"),
					Resp.IsValid() ? Resp->GetResponseCode() : 0);
				return;
			}

			const FString Body = Resp->GetContentAsString();
			const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Body);
			TSharedPtr<FJsonObject> Json;
			if (!FJsonSerializer::Deserialize(Reader, Json) || !Json.IsValid())
			{
				UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("Invalid relay credential response"));
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
				if (!ConfigureError.IsEmpty())
				{
					UE_LOG(LogWeaponCentralConnectivity, Warning,
						TEXT("Relay NetDriver config deferred: %s"), *ConfigureError);
				}
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

	// Host 侧若配置了内网 TURN 地址,用它直连,不经路由器 hairpin 绕路。
	// 客户端仍用协调器下发的公网地址。
	const FString RelayServerAddr = (World->GetNetMode() == NM_ListenServer && !HostRelayServerAddress.IsEmpty())
		? HostRelayServerAddress
		: RelayCredentialsCached->RelayAddr;

	RelayDriver->SetRelayConnectionInfo(
		RelayServerAddr,
		RelayCredentialsCached->Username,
		RelayCredentialsCached->Password);

	// Host:若 Listen socket 在凭证到达前已创建(未走 TURN),热升级为
	// relay socket(RecreateSocket 仅客户端可用,Host 用 UpgradeSocketToRelay)。
	if (World->GetNetMode() == NM_ListenServer)
	{
		const bool bUpgraded = RelayDriver->UpgradeSocketToRelay();
		UE_LOG(LogWeaponCentralConnectivity, Log,
			TEXT("Relay NetDriver host upgrade: %s (socket=%s)"),
			bUpgraded ? TEXT("ok") : TEXT("failed"),
			RelayDriver->IsSocketRelay() ? TEXT("WeaponRelay") : TEXT("direct"));
	}

	UE_LOG(LogWeaponCentralConnectivity, Log,
		TEXT("Relay NetDriver configured with TURN server %s"), *RelayServerAddr);
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
	if (!bReady && !HostMigration->IsMigrationBackendCommitted())
	{
		// Central startup converges in stages: authentication completes in the menu,
		// while the Host Lease and first checkpoint only become ready after the
		// gameplay Listen Server starts. Publishing Local readiness during those
		// intermediate stages would permanently latch this Match to LocalP2P before
		// Central has had a chance to finish its handshake.
		UE_LOG(
			LogWeaponCentralConnectivity,
			Verbose,
			TEXT("Central backend pending (control=%d lease=%d checkpoint=%d)"),
			bCentralControlReady,
			bCentralLeaseReady,
			bCentralCheckpointReady);
		return;
	}

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

	// WebSockets 是引擎 runtime 模块;打包版不会自动预加载(编辑器因其他模块
	// 间接加载)。GetModuleChecked 在未加载时 assert 崩溃,故用 LoadModule:
	// 未加载则尝试加载,模块被裁剪时返回 nullptr,优雅降级禁用事件通道。
	FWebSocketsModule* WsModule = FModuleManager::Get().LoadModulePtr<FWebSocketsModule>(TEXT("WebSockets"));
	if (!WsModule)
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning,
			TEXT("OpenEventChannel skipped: WebSockets module unavailable in this build"));
		return;
	}
	// WS 握手 = HTTP 升级,必须带 Bearer token(Go 侧 events 路由 RequireDevice)
	TMap<FString, FString> UpgradeHeaders;
	if (!AccessToken.IsEmpty())
	{
		UpgradeHeaders.Add(TEXT("Authorization"), FString::Printf(TEXT("Bearer %s"), *AccessToken));
	}
	EventSocket = WsModule->CreateWebSocket(WsUrl, TEXT(""), UpgradeHeaders);
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
	UWeaponRelayNetDriver* RelayDriver = Cast<UWeaponRelayNetDriver>(NetDriver);
	if (!RelayDriver)
	{
		UE_LOG(LogWeaponCentralConnectivity, Verbose,
			TEXT("TickRelayNetDriver: no WeaponRelayNetDriver (world=%s netdriver=%s)"),
			World ? *World->GetName() : TEXT("null"),
			NetDriver ? *NetDriver->GetClass()->GetName() : TEXT("null"));
		return;
	}

	// Host 自愈:凭证可能在 Listen socket 创建后才异步到达。此时 socket 已是
	// 纯 UDP,需在进入玩法 World(NetDriver 可用)后补一次 relay 热升级。
	// 判据是 socket 是否已包装为 WeaponRelay(而非凭证是否注入——SetRelay
	// ConnectionInfo 在菜单阶段已置位,与 socket 是否升级无关)。
	// ConfigureRelayNetDriver/UpgradeSocketToRelay 幂等,可安全重复调用。
	if (CurrentMatchRole == TEXT("Host") && RelayCredentialsCached.IsValid() &&
		!RelayDriver->IsSocketRelay())
	{
		FString ConfigureError;
		ConfigureRelayNetDriver(ConfigureError);
		if (!ConfigureError.IsEmpty())
		{
			UE_LOG(LogWeaponCentralConnectivity, Warning,
				TEXT("Relay NetDriver self-heal failed: %s"), *ConfigureError);
		}
	}

	RelayDriver->TickRelay();
}

void UWeaponCentralConnectivitySubsystem::HandleLeaseGranted(const FWeaponCentralHostLease& Lease)
{
	// Lease 已通过 fencing;置第二门锁并报告事实
	AuthorityEpoch = Lease.AuthorityEpoch;
	bCentralLeaseReady = true;
	UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Central lease ready (epoch %llu)"), Lease.AuthorityEpoch);
	PublishBackendReadiness();
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
