// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "WeaponCentralConnectivityTypes.h"
#include "WeaponHostMigrationTypes.h"
#include "Engine/TimerHandle.h"
#include "EventLoop/EventLoopTimer.h"
#include "WeaponCentralConnectivitySubsystem.generated.h"

class UWeaponHostMigrationSubsystem;
class UWeaponSessionSubsystem;
class UWeaponSnapshotCoordinator;
class IWebSocket;
class FJsonObject;
struct FWeaponMigrationBackendReadiness;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FOnWeaponCentralConnectivityChanged,
	EWeaponCentralConnectivityState, NewState,
	FString, Message);

/**
 * 中心控制面适配器:只把中心已验证的事实(Readiness / Lease / Endpoint)
 * 交给现有系统,不直接改 GameMode,不推进迁移状态机。
 *
 * 决策权始终在 UWeaponHostMigrationSubsystem;本子系统是外围传感器。
 */
UCLASS(Config = Game, DefaultConfig)
class LETSDEEPROCK_API UWeaponCentralConnectivitySubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	EWeaponCentralConnectivityState GetConnectivityState() const { return ConnectivityState; }

	/** 触发完整中心接入流程(设备身份 → 控制连接 → Lease → 候选 → readiness)。 */
	UFUNCTION(BlueprintCallable, Category = "Central Connectivity")
	void StartCentralConnect();

	UFUNCTION(BlueprintPure, Category = "Central Connectivity")
	bool IsCentralControlReady() const { return bCentralControlReady; }

	UFUNCTION(BlueprintPure, Category = "Central Connectivity")
	bool IsCentralLeaseReady() const { return bCentralLeaseReady; }

	UFUNCTION(BlueprintPure, Category = "Central Connectivity")
	bool IsCentralCheckpointReady() const { return bCentralCheckpointReady; }

	UPROPERTY(BlueprintAssignable, Category = "Central Connectivity")
	FOnWeaponCentralConnectivityChanged OnConnectivityChanged;

	/** Host: 上传一个候选到中心。 */
	bool UploadCandidate(const FWeaponCentralEndpointCandidate& Candidate, FString& OutError) const;

	/** Host: 用中心已确认的 direct endpoint 触发客户端 reconnect。 */
	bool TriggerDirectReconnect(const FString& Address, FString& OutError);

	/** Host: 创建 Match(中心分配 MatchId + JoinCode)。 */
	bool CreateCentralMatch(FString& OutError);

	/** Client: 用 JoinCode 加入 Match。 */
	bool JoinCentralMatch(const FString& JoinCode, FString& OutError);

private:
	// 事实输入管线
	void RunAuthPipeline();                 // challenge → register → session
	void SendChallengeRequest();            // POST /v1/devices/challenge
	void SendRegisterRequest(const FString& Nonce); // POST /v1/devices/register
	void SendSessionRequest(const FString& Nonce, const TArray<uint8>& Signature); // POST /v1/devices/session
	void HandleSessionSuccess(const FString& Token);
	void FailAuthPipeline(const FString& Message);

	// Match 生命周期(中心分配 MatchId)
	void SendCreateMatchRequest();
	void SendJoinMatchRequest(const FString& JoinCode);
	void HandleMatchCreated(const FGuid& InMatchId, const FString& RawMatchId, const FString& JoinCode);
	void HandleMatchJoined(const FGuid& InMatchId, const FString& RawMatchId, const FString& Role);
	void ExecutePendingMatchAction();

	// 待认证完成后的 Match 动作(异步认证与 Match 请求脱节)
	enum class EPendingMatchAction : uint8
	{
		None,
		Create,
		Join,
	};
	EPendingMatchAction PendingMatchAction = EPendingMatchAction::None;
	FString PendingJoinCode;

	// 向现有系统交付事实
	void PublishBackendReadiness();

	// WebSocket 事件通道
	void OpenEventChannel();
	void HandleEventMessage(const FString& Message);
	void HandleEventDispatch(const TSharedPtr<FJsonObject>& Json, const FString& EventType, const FGuid& EventMatchId, uint64 EventEpoch);
	void HandleLeaseGranted(const FWeaponCentralHostLease& Lease);
	void HandleHostEndpointNominated(const FWeaponCentralEndpointCandidate& Endpoint);
	void HandleCheckpointCommitted(const FGuid& InMatchId, uint64 Epoch, int64 Revision);
	void ScheduleReconnect();
	void HandleReconnectTimeout();
	bool ValidateEventFence(const FGuid& EventMatchId, uint64 EventEpoch);
	void HandleEventStashed(const FString& RawEvent);
	void ReplayStashedEvents();

	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity|Reconnect")
	float ReconnectBaseDelaySeconds = 1.0f;

	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity|Reconnect")
	float ReconnectMaxDelaySeconds = 30.0f;

	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity|Reconnect")
	float ReconnectTimeoutSeconds = 60.0f;

	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity")
	FString CoordinatorBaseUrl = TEXT("http://192.168.3.75:8443");

	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity")
	FString StunServerUrl = TEXT("stun:coordinator.example.com:3478");

	UPROPERTY()
	TArray<uint8> DevicePrivateKey;

	UPROPERTY()
	TArray<uint8> DevicePublicKey;

	UPROPERTY()
	FString PendingChallengeNonce;

	UPROPERTY()
	FString DeviceId;

	UPROPERTY()
	FString AccessToken;

	// 非 UObject 智能指针成员:不能是 UPROPERTY(UHT 不认模板),纯 C++ 成员即可
	TSharedPtr<IWebSocket> EventSocket;

	FTimerHandle ReconnectTimerHandle;
	int32 ReconnectAttempt = 0;
	double ReconnectDeadlineSeconds = 0.0;

	// 暂存缓冲区:只存事件原文,不存判断结论;MatchId 建立后重放时走完整 fence
	TArray<FString> StashedEvents;

	UPROPERTY()
	FGuid MatchId;

	// Go 返回的原始 matchId 字符串(小写带连字符 uuid)。
	// 不能经 FGuid 往返:ToString() 会变成大写无连字符,查库 404。
	UPROPERTY()
	FString CentralMatchIdString;

	UPROPERTY()
	uint64 AuthorityEpoch = 0;

	UPROPERTY()
	uint64 RosterRevision = 0;

	// 当前 Lease 的 Host 设备身份(来自 LeaseGranted 事件顶层 peerId);
	// 用于校验 HostEndpointPublished 事件的身份一致性。
	UPROPERTY()
	FString ExpectedHostPeerId;

	EWeaponCentralConnectivityState ConnectivityState = EWeaponCentralConnectivityState::Disconnected;
	EWeaponCentralAuthState AuthState = EWeaponCentralAuthState::None;
	bool bCentralControlReady = false;
	bool bCentralLeaseReady = false;
	bool bCentralCheckpointReady = false;

	TWeakObjectPtr<UWeaponHostMigrationSubsystem> HostMigration;
	TWeakObjectPtr<UWeaponSessionSubsystem> SessionSubsystem;
	TWeakObjectPtr<UWeaponSnapshotCoordinator> SnapshotCoordinator;
};
