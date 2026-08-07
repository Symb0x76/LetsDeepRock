// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponCentralConnectivitySubsystem.h"

#include "WeaponCentralDeviceCrypto.h"
#include "WeaponHostMigrationSubsystem.h"
#include "WeaponSessionSubsystem.h"
#include "WeaponSnapshotCoordinator.h"

#include "HttpModule.h"
#include "Interfaces/IHttpRequest.h"
#include "Interfaces/IHttpResponse.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "HAL/PlatformFileManager.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Misc/Base64.h"

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

	ConnectivityState = EWeaponCentralConnectivityState::Authenticating;
	AuthState = EWeaponCentralAuthState::None;
	OnConnectivityChanged.Broadcast(ConnectivityState, TEXT("Starting central authentication"));

	// 第 2 课实现:RunAuthPipeline() 发起 challenge → register → session。
	// 第 1 课只建立边界:适配器不在这里做任何决策,只把事实交给 HostMigration。
	RunAuthPipeline();
}

bool UWeaponCentralConnectivitySubsystem::UploadCandidate(const FWeaponCentralEndpointCandidate& Candidate, FString& OutError)
{
	// 第 3 课实现:POST /v1/matches/{matchId}/peers/{peerId}/candidates
	OutError = TEXT("Not implemented until connectivity lesson");
	return false;
}

bool UWeaponCentralConnectivitySubsystem::TriggerDirectReconnect(const FString& Address, FString& OutError)
{
	// 第 3 课实现:验证 Match/Epoch 后调用 SessionSubsystem->ReconnectToMigratedHost(Address)
	OutError = TEXT("Not implemented until connectivity lesson");
	return false;
}

// —— 第 1 课核心:向现有系统交付事实 ——

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

// 第 2 课:HTTP 设备身份管线(challenge → register → session)

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
	Request->SetContentAsString(Body);

	Request->OnProcessRequestComplete().BindLambda([this, Nonce](FHttpRequestPtr Req, const FHttpResponsePtr& Resp, bool bWasSuccessful)
	{
		if (!bWasSuccessful || !Resp.IsValid())
		{
			FailAuthPipeline(TEXT("Register request failed"));
			return;
		}

		const int32 StatusCode = Resp->GetResponseCode();
		// 200(已存在)/201(新注册)都是成功;其他(如 400/409 非幂等路径)失败
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
	const FString SignatureB64 = FBase64::Encode(Signature, EBase64Mode::UrlSafe);
	const FString Body = FString::Printf(
		TEXT("{\"nonce\":\"%s\",\"signature\":\"%s\"}"),
		*Nonce, *SignatureB64);

	FHttpModule& Http = FHttpModule::Get();
	TSharedRef<IHttpRequest> Request = Http.CreateRequest();
	Request->SetURL(CoordinatorBaseUrl + TEXT("/v1/devices/session"));
	Request->SetVerb(TEXT("POST"));
	Request->SetHeader(TEXT("Content-Type"), TEXT("application/json"));
	Request->SetContentAsString(Body);

	Request->OnProcessRequestComplete().BindLambda([this](FHttpRequestPtr Req, const FHttpResponsePtr& Resp, bool bWasSuccessful)
	{
		if (!bWasSuccessful || !Resp.IsValid())
		{
			FailAuthPipeline(TEXT("Session request failed"));
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

	// 第 1 课:把"控制连接就绪"事实交给状态机
	PublishBackendReadiness();

	// 第 3 课:打开 WebSocket 事件通道
	OpenEventChannel();
}

void UWeaponCentralConnectivitySubsystem::FailAuthPipeline(const FString& Message)
{
	AuthState = EWeaponCentralAuthState::Failed;
	ConnectivityState = EWeaponCentralConnectivityState::Failed;
	UE_LOG(LogWeaponCentralConnectivity, Warning, TEXT("Auth pipeline failed: %s"), *Message);
	OnConnectivityChanged.Broadcast(ConnectivityState, Message);
}

void UWeaponCentralConnectivitySubsystem::OpenEventChannel()
{
	// 第 3 课:WebSocket 事件通道
}

void UWeaponCentralConnectivitySubsystem::HandleLeaseGranted(const FWeaponCentralHostLease& Lease)
{
	// 第 3 课:校验 Match/Epoch 后置 bCentralLeaseReady,调用 PublishBackendReadiness()
}

void UWeaponCentralConnectivitySubsystem::HandleHostEndpointNominated(const FWeaponCentralEndpointCandidate& Endpoint)
{
	// 第 3 课:校验 Epoch 后调用 TriggerDirectReconnect(Endpoint.Address)
}

void UWeaponCentralConnectivitySubsystem::HandleCheckpointCommitted(const FGuid& InMatchId, uint64 Epoch, int64 Revision)
{
	// 第 3 课:校验后置 bCentralCheckpointReady,调用 PublishBackendReadiness()
}
