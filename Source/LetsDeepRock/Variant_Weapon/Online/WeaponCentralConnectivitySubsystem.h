// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "WeaponCentralConnectivityTypes.h"
#include "WeaponHostMigrationTypes.h"
#include "Engine/TimerHandle.h"
#include "EventLoop/EventLoopTimer.h"
#include "WeaponCentralConnectivitySubsystem.generated.h"

class FWeaponCandidateProber;
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

	/** 控制面已认证且 Coordinator 已为本局分配 MatchId。 */
	UFUNCTION(BlueprintPure, Category = "Central Connectivity")
	bool IsCentralMatchReady() const { return bCentralControlReady && MatchId.IsValid() && !CentralMatchIdString.IsEmpty(); }

	/** 当前中心的 MatchId（原始小写 uuid 字符串；未创建/加入 Match 时为空）。 */
	UFUNCTION(BlueprintPure, Category = "Central Connectivity")
	const FString& GetCentralMatchIdString() const { return CentralMatchIdString; }

	/** 玩家可分享/输入的中心房间号；与内部 MatchId 分离。 */
	UFUNCTION(BlueprintPure, Category = "Central Connectivity")
	const FString& GetCentralJoinCode() const { return CentralJoinCode; }

	UPROPERTY(BlueprintAssignable, Category = "Central Connectivity")
	FOnWeaponCentralConnectivityChanged OnConnectivityChanged;

	/** Host: 上传一个候选到中心。 */
	bool UploadCandidate(const FWeaponCentralEndpointCandidate& Candidate, FString& OutError) const;

	/** Host: 用中心已确认的 direct endpoint 触发客户端 reconnect。 */
	bool TriggerDirectReconnect(const FString& Address, FString& OutError);

	/** 向 Coordinator 请求本 peer 的短期 TURN relay 凭证并缓存,供 ConfigureRelayNetDriver 使用。 */
	bool RequestRelayCredentials(FString& OutError);

	/** 用缓存的 relay 凭证配置当前 NetDriver(连接前调用)。 */
	bool ConfigureRelayNetDriver(FString& OutError);

	/** 是否已缓存有效的 relay 凭证。 */
	bool HasRelayCredentials() const { return RelayCredentialsCached.IsValid(); }

	/** 读取缓存的 relay 凭证(客户端 pending 阶段创建 socket 时用,不依赖 NetDriver)。 */
	bool GetCachedRelayCredentials(FString& OutTurnServerAddr, FString& OutUsername, FString& OutPassword) const
	{
		if (!RelayCredentialsCached.IsValid())
		{
			return false;
		}
		OutTurnServerAddr = RelayCredentialsCached->RelayAddr;
		OutUsername = RelayCredentialsCached->Username;
		OutPassword = RelayCredentialsCached->Password;
		return !OutTurnServerAddr.IsEmpty();
	}

	/** 客户端已决定走 relay 回退(仅客户端有意义;Host 始终有凭证即走 relay)。 */
	bool ShouldUseRelayFallback() const { return bRelayFallbackRequested; }

	/** 每帧推进 relay NetDriver 的 TURN 事务(重传/超时)。 */
	void TickRelayNetDriver();

	/** Host: 创建 Match(中心分配 MatchId + JoinCode)。 */
	bool CreateCentralMatch(FString& OutError);

	/** Client: 用 JoinCode 加入 Match。 */
	bool JoinCentralMatch(const FString& JoinCode, FString& OutError);

private:
	// 事实输入管线
	bool NormalizeAndValidateCoordinatorBaseUrl(FString& OutError);
	void RunAuthPipeline();                 // challenge → register → session
	void SendChallengeRequest();            // POST /v1/devices/challenge
	void SendRegisterRequest(const FString& Nonce); // POST /v1/devices/register
	void SendSessionRequest(const FString& Nonce, const TArray<uint8>& Signature); // POST /v1/devices/session
	void HandleSessionSuccess(const FString& Token);
	void FailAuthPipeline(const FString& Message);

	// Match 生命周期(中心分配 MatchId)
	void SendCreateMatchRequest();
	void SendJoinMatchRequest(const FString& JoinCode);
	void HandleMatchCreated(
		const FGuid& InMatchId,
		const FString& RawMatchId,
		const FString& JoinCode,
		const FString& HostPeerId,
		uint64 InAuthorityEpoch,
		uint64 InRosterRevision);
	void HandleMatchJoined(
		const FGuid& InMatchId,
		const FString& RawMatchId,
		const FString& Role,
		const FString& JoinCode,
		const FString& HostPeerId,
		uint64 InAuthorityEpoch,
		uint64 InRosterRevision);
	void ExecutePendingMatchAction();
	void FailMatchAction(const FString& Message);
	void ResetMatchTransportState();

	// Host endpoint publication: listen Session ready -> Lease -> candidate -> nomination.
	void HandleLanHostComplete(bool bWasSuccessful, const FString& Message);
	void ClaimHostLease();
	void SendLeaseHeartbeat();
	void PublishHostEndpoint();
	void UploadHostCandidate(const FString& CandidateType, const FString& Address, int32 Port, uint32 Priority, const FString& Foundation);
	void QueryStunReflexiveAddress(FString& OutAddress, int32& OutPort);
	FString GetRelayedHostCandidateAddress() const;
	void PublishRelayedHostCandidate();
	void NominateHostEndpoint(const FString& Address, int32 Port);
	bool ResolveHostEndpoint(FString& OutAddress, int32& OutPort, FString& OutError) const;
	// 延迟到下一帧执行阻塞 3s 的 STUN 查询,避免卡住 Lease 回调;结果缓存后每次 heartbeat 复用。
	void ScheduleReflexivePublish();
	void QueryAndPublishReflexive();

	// Late join cannot depend on an already-consumed WebSocket event; poll persisted candidates.
	void BeginHostEndpointWait();
	void RequestHostCandidates();
	void ScheduleHostCandidatePoll();
	void StopHostEndpointWait();
	void HandleHostEndpointWaitTimeout();
	static FString FormatEndpointAddress(const FString& Address, int32 Port);

	// 客户端候选试探:按优先级逐个 travel 候选,连不通自动推进下一个,最后回退 relay。
	void BeginCandidateProbe();
	// 并行测速(ping 全部候选)结束后按 RTT 重排队列,再逐个 travel。
	void TickCandidatePing();
	void ApplyCandidatePingResults();
	void ProbeNextCandidate();
	void HandleCandidateProbeTimeout();
	void HandleCandidateProbeFailure();
	void HandleCandidateProbeSucceeded();
	void ClearCandidateProbe(bool bTerminal, const FString& Message);
	void BuildCandidateQueue(const TArray<TSharedPtr<FJsonValue>>& Candidates);
	// 连接失败感知:engine 全局回调 + travel 完成信号,与 HostMigration 的回调各自按状态过滤。
	void HandleNetworkFailure(UWorld* World, UNetDriver* NetDriver, ENetworkFailure::Type FailureType, const FString& ErrorString);
	void HandlePostLoadMap(UWorld* LoadedWorld);

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
	static int32 RankDirectCandidate(const FWeaponCentralEndpointCandidate& Candidate);
	void HandleCheckpointCommitted(const FGuid& InMatchId, uint64 Epoch, int64 Revision);
	void ScheduleReconnect();
	void HandleReconnectTimeout();
	bool ValidateEventFence(const FGuid& EventMatchId, uint64 EventEpoch);
	void HandleEventStashed(const FString& RawEvent);
	void ReplayStashedEvents();
	void ResetEventSocket();

	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity|Reconnect")
	float ReconnectBaseDelaySeconds = 1.0f;

	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity|Reconnect")
	float ReconnectMaxDelaySeconds = 30.0f;

	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity|Reconnect")
	float ReconnectTimeoutSeconds = 60.0f;

	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity")
	FString CoordinatorBaseUrl = TEXT("http://symb0x76.top:8444");

	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity")
	FString StunServerUrl = TEXT("stun:symb0x76.top:3478");

	/**
	 * Host 侧直连 TURN 服务器的内网地址(host:port)。
	 * 留空时 Host 与客户端一样用协调器下发的公网 RelayAddr。TURN 与 Host 同局域网
	 * 时可设为内网 TURN 地址(如 192.168.3.27:3478),省掉经路由器 hairpin 的绕路;
	 * 仅当路由器不支持 hairpin 时才是必需的。
	 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity")
	FString HostRelayServerAddress;
	/** Optional public DNS/IP for the listen Host. Blank falls back to the local interface address (LAN only). */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity|Host Endpoint")
	FString AdvertisedHostAddress;

	/** Optional externally forwarded UDP port. Zero uses the actual IpNetDriver listen port. */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity|Host Endpoint")
	int32 AdvertisedHostPort = 0;

	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity|Host Endpoint")
	float LeaseHeartbeatIntervalSeconds = 10.0f;

	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity|Host Endpoint")
	float HostEndpointPollIntervalSeconds = 1.0f;

	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity|Host Endpoint")
	float HostEndpointWaitTimeoutSeconds = 20.0f;

	/** 单个 direct 候选的试探超时(秒);同时作为该候选 travel URL 的 InitialConnectTimeout/ConnectionTimeout。 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity|Candidate Probe")
	float CandidateProbeTimeoutSeconds = 10.0f;

	/** relay 候选的试探超时(秒);TURN Allocate + 握手比 direct 慢。 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity|Candidate Probe")
	float RelayProbeTimeoutSeconds = 15.0f;

	/** travel 前并行 ping 全部候选的最长时间窗(秒);全部应答则提前结束。0 关闭测速。 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity|Candidate Probe")
	float CandidatePingWindowSeconds = 2.0f;

	/** 把阻塞 3s 的 STUN 反射查询延迟到下一帧执行,避免卡住 Lease 回调。 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity|Host Endpoint")
	float ReflexivePublishDelaySeconds = 0.1f;

	/** 纯 LAN 局可关闭 STUN 反射查询(Host 只发内网候选)。 */
	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity|Host Endpoint")
	bool bStunQueryEnabled = true;

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
	FString EventSocketMatchIdString;

	FTimerHandle ReconnectTimerHandle;
	FTimerHandle LeaseHeartbeatTimerHandle;
	FTimerHandle HostEndpointPollTimerHandle;
	FTimerHandle HostEndpointWaitTimerHandle;
	FTimerHandle RelayTickTimerHandle;
	FTimerHandle CandidateProbeTimeoutTimerHandle;
	FTimerHandle CandidatePingTimerHandle;
	// 非 UObject;TSharedPtr 允许头文件只前置声明(删除器在创建处绑定)
	TSharedPtr<FWeaponCandidateProber> CandidatePinger;
	FTimerHandle ReflexivePublishTimerHandle;
	int32 ReconnectAttempt = 0;
	double ReconnectDeadlineSeconds = 0.0;

	// 连接失败感知:engine 全局回调句柄(与 HostMigration 各自挂载、按状态过滤)
	FDelegateHandle NetworkFailureDelegateHandle;
	FDelegateHandle PostLoadMapDelegateHandle;

	// 客户端候选试探:按优先级排序的候选队列 + 当前尝试索引
	TArray<FWeaponCentralEndpointCandidate> CandidateQueue;
	int32 CandidateProbeIndex = INDEX_NONE;
	bool bCandidateProbeActive = false;
	bool bRelayProbeInProgress = false;
	FString PendingCandidateProbeAddress;

	// Host 侧 STUN 反射结果缓存(只查询一次,每次 heartbeat 复用)
	FString CachedReflexiveAddress;
	int32 CachedReflexivePort = 0;
	bool bStunQueryCompleted = false;

	// 暂存缓冲区:只存事件原文,不存判断结论;MatchId 建立后重放时走完整 fence
	TArray<FString> StashedEvents;

	// Coordinator 下发的短期 TURN relay 凭证缓存(Relay 路径 fallback 用)
	struct FRelayCredentialsCache
	{
		FString Username;
		FString Password;
		FString RelayAddr;
	};
	TSharedPtr<FRelayCredentialsCache> RelayCredentialsCached;

	UPROPERTY()
	FGuid MatchId;

	// Go 返回的原始 matchId 字符串(小写带连字符 uuid)。
	// 不能经 FGuid 往返:ToString() 会变成大写无连字符,查库 404。
	UPROPERTY()
	FString CentralMatchIdString;

	/** Coordinator 分配给玩家使用的短 JoinCode，不参与协议 fencing。 */
	UPROPERTY()
	FString CentralJoinCode;

	UPROPERTY()
	FString CurrentMatchRole;

	UPROPERTY()
	FString PublishedCandidateId;

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
	bool bCreatedCentralMatch = false;
	bool bLeaseClaimInFlight = false;
	bool bCandidateRequestInFlight = false;
	bool bWaitingForHostEndpoint = false;
	bool bCentralControlReady = false;
	bool bCentralLeaseReady = false;
	bool bCentralCheckpointReady = false;
	// 客户端已选定 relay 候选并准备经 TURN 连接(仅客户端有意义)
	bool bRelayFallbackRequested = false;

	TWeakObjectPtr<UWeaponHostMigrationSubsystem> HostMigration;
	TWeakObjectPtr<UWeaponSessionSubsystem> SessionSubsystem;
	TWeakObjectPtr<UWeaponSnapshotCoordinator> SnapshotCoordinator;
};
