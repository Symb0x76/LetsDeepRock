// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponCentralConnectivitySubsystem.h"

#include "WeaponCentralDeviceCrypto.h"
#include "WeaponHostMigrationSubsystem.h"
#include "WeaponSessionSubsystem.h"
#include "WeaponSnapshotCoordinator.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "PlatformHttp.h"
#include "IWebSocket.h"
#include "WebSocketsModule.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Base64.h"
#include "TimerManager.h"

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
	}

	ConnectivityState = EWeaponCentralConnectivityState::Disconnected;
	UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Central Connectivity subsystem initialized"));
}

void UWeaponCentralConnectivitySubsystem::Deinitialize()
{
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
	if (!MatchId.IsValid())
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
		*CoordinatorBaseUrl, *MatchId.ToString(), *Candidate.PeerId));
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
	if (JoinCode.IsEmpty())
	{
		OutError = TEXT("Empty join code");
		return false;
	}
	if (AccessToken.IsEmpty())
	{
		// 认证尚未完成:排队,HandleSessionSuccess 后执行
		PendingMatchAction = EPendingMatchAction::Join;
		PendingJoinCode = JoinCode;
		return true;
	}
	SendJoinMatchRequest(JoinCode);
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
		HandleMatchCreated(CreatedMatchId, RawMatchId, JoinCode);
	});
	Request->ProcessRequest();
}

void UWeaponCentralConnectivitySubsystem::FailMatchAction(const FString& Message)
{
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

	Request->OnProcessRequestComplete().BindLambda([this](FHttpRequestPtr Req, const FHttpResponsePtr& Resp, bool bWasSuccessful)
	{
		if (!bWasSuccessful || !Resp.IsValid() || Resp->GetResponseCode() != 200)
		{
			UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("JoinMatch failed (HTTP %d): %s"),
				Resp.IsValid() ? Resp->GetResponseCode() : 0,
				Resp.IsValid() ? *Resp->GetContentAsString() : TEXT("(no response)"));
			return;
		}

		TSharedPtr<FJsonObject> Json;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Resp->GetContentAsString());
		if (!FJsonSerializer::Deserialize(Reader, Json))
		{
			UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("JoinMatch response parse failed"));
			return;
		}

		const TSharedPtr<FJsonObject>* MatchObj = nullptr;
		if (!Json->TryGetObjectField(TEXT("match"), MatchObj) || !MatchObj->IsValid())
		{
			UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("JoinMatch response missing match"));
			return;
		}

		// 保留原始 matchId 字符串(Go uuid 格式),不经 FGuid 往返
		const FString RawMatchId = (*MatchObj)->GetStringField(TEXT("matchId"));
		const FGuid JoinedMatchId(RawMatchId);
		const FString Role = Json->GetStringField(TEXT("role"));
		HandleMatchJoined(JoinedMatchId, RawMatchId, Role);
	});
	Request->ProcessRequest();
}

void UWeaponCentralConnectivitySubsystem::HandleMatchCreated(const FGuid& InMatchId, const FString& RawMatchId, const FString& JoinCode)
{
	if (!InMatchId.IsValid() || RawMatchId.IsEmpty())
	{
		FailMatchAction(TEXT("Create Match returned an invalid MatchId"));
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

	MatchId = InMatchId;
	CentralMatchIdString = RawMatchId;
	UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Central match created: %s (join code %s)"),
		*CentralMatchIdString, *JoinCode);

	// MatchId 建立 → 重放暂存事件 + 打开事件通道
	ReplayStashedEvents();
	OpenEventChannel();

	// 创建 Host 的 UI 只需要控制面认证 + MatchId。Lease / 首个 Checkpoint
	// 必须进入玩法世界后才能继续收敛，不能反向阻塞 Host 创建。
	ConnectivityState = EWeaponCentralConnectivityState::Connected;
	OnConnectivityChanged.Broadcast(ConnectivityState, TEXT("Central Match ready"));
}

void UWeaponCentralConnectivitySubsystem::HandleMatchJoined(const FGuid& InMatchId, const FString& RawMatchId, const FString& Role)
{
	if (!InMatchId.IsValid())
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("JoinMatch returned invalid MatchId"));
		return;
	}

	MatchId = InMatchId;
	CentralMatchIdString = RawMatchId;
	UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Central match joined: %s (role %s)"),
		*CentralMatchIdString, *Role);

	// MatchId 建立 → 重放暂存事件 + 打开事件通道
	ReplayStashedEvents();
	OpenEventChannel();
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
	const FString DeviceKeyFileName = TEXT("DeviceKey.bin");

	FString GetDeviceKeyPath()
	{
		return FPaths::Combine(FPaths::ProjectSavedDir(), DeviceKeyFileName);
	}

	bool LoadDeviceKeyPair(TArray<uint8>& OutPrivateKey, TArray<uint8>& OutPublicKey)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		const FString Path = GetDeviceKeyPath();
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

	bool SaveDeviceKeyPair(const TArray<uint8>& PrivateKey, const TArray<uint8>& PublicKey)
	{
		IPlatformFile& PlatformFile = FPlatformFileManager::Get().GetPlatformFile();
		const FString Path = GetDeviceKeyPath();

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
		if (LoadDeviceKeyPair(DevicePrivateKey, DevicePublicKey))
		{
			UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Device key pair loaded from disk"));
		}
		else
		{
			if (!FWeaponCentralDeviceCrypto::GenerateKeyPair(DevicePrivateKey, DevicePublicKey))
			{
				FailAuthPipeline(TEXT("Failed to generate device key pair"));
				return;
			}
			if (!SaveDeviceKeyPair(DevicePrivateKey, DevicePublicKey))
			{
				FailAuthPipeline(TEXT("Failed to persist device key pair"));
				return;
			}
			UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Device key pair generated and persisted"));
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

	// 打开 WebSocket 事件通道
	OpenEventChannel();

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
	// 防重入:已有活跃 socket 时跳过(避免多调用点反复断开重连)
	if (EventSocket.IsValid())
	{
		return;
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

	EventSocket->OnConnected().AddLambda([this]()
	{
		ReconnectAttempt = 0;
		ConnectivityState = EWeaponCentralConnectivityState::Connected;
		UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Event channel connected"));
		OnConnectivityChanged.Broadcast(ConnectivityState, TEXT("Event channel connected"));
	});

	EventSocket->OnConnectionError().AddLambda([this](const FString& Error)
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("Event channel error: %s"), *Error);
		ScheduleReconnect();
	});

	EventSocket->OnClosed().AddLambda([this](int32 StatusCode, const FString& Reason, bool bWasClean)
	{
		UE_LOG(LogWeaponCentralConnectivity, Log, TEXT("Event channel closed: %d %s"), StatusCode, *Reason);
		ScheduleReconnect();
	});

	EventSocket->OnMessage().AddLambda([this](const FString& Message)
	{
		HandleEventMessage(Message);
	});

	EventSocket->Connect();
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
		HandleHostEndpointNominated(Endpoint);
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
	// 已通过 fencing 的 direct endpoint → 触发 reconnect
	FString Error;
	if (!TriggerDirectReconnect(Endpoint.Address, Error))
	{
		UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("Reconnect failed: %s"), *Error);
		return;
	}
	ConnectivityState = EWeaponCentralConnectivityState::DirectConnected;
	OnConnectivityChanged.Broadcast(ConnectivityState, TEXT("Direct endpoint connected"));
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
	if (EventSocket.IsValid())
	{
		EventSocket.Reset();
	}

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
