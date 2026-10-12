// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sockets.h"
#include "HAL/CriticalSection.h"
#include "WeaponRelaySocket.generated.h"

class FWeaponRelaySocket;

/**
 * 状态码:Relay 路径的分配/数据面状态。
 */
UENUM()
enum class EWeaponRelaySocketState : uint8
{
	Idle,
	Allocating,          // Allocate 事务进行中
	Ready,               // 已分配 relayed address,数据面可用(按 peer 走 Send Indication / ChannelData)
	Failed,              // 不可恢复错误
	Closed,
};

/**
 * FWeaponRelaySocket 把 TURN 服务器当作 peer:它是一个"虚拟 FSocket"。
 *
 * UE 的 IpNetDriver/NetConnection 通过 FSocket 收发字节;本类把 FSocket
 * 接口映射到 TURN 数据面:
 *   - 底层持有真正的 UDP FSocket(直连 TURN 服务器)
 *   - SendTo(Data, Dest)  -> 每个 Dest 一个 peer:未绑定走 Send Indication,
 *                            ChannelBind 成功后走 ChannelData(各 peer 独立 channel)
 *   - RecvFrom 解出服务器转发的 peer 数据,Source 为真实 peer 地址
 *   - 非 TURN 服务器来源的 UDP 视为直连流量,原样上交并记住该 peer 走直连
 *   - GetAddress 返回虚拟的 relayed address(对端看到的"我")
 *
 * 生命周期:Allocation 每 RefreshInterval 刷新;permission/channel 每
 * PermissionRefreshInterval 重新 ChannelBind/CreatePermission(RFC 5766:
 * permission 5 分钟、channel 10 分钟过期)。所有请求带重传与 438 nonce 续期。
 *
 * Gameplay 层只看到 FSocket 语义,完全不知道数据走了中转。
 */
class LETSDEEPROCK_API FWeaponRelaySocket : public FSocket
{
public:
	FWeaponRelaySocket(FSocket* InUnderlyingSocket, const FString& InDescription);
	virtual ~FWeaponRelaySocket() override;

	// ---- TURN 控制面(在 Game 线程调用) ----

	/** 发起 Allocate,成功后分配 relayed address。 */
	bool Allocate(const FString& TurnServerAddr, const FString& Username, const FString& Password);

	/** 由外部轮询驱动的 TURN 事务推进(每帧调用,处理重传/超时/刷新)。 */
	void Tick();

	/** 底层 socket 的可读性(包装 HasPendingData)。 */
	bool HasPendingRelayData(uint32& PendingDataSize);

	// ---- FSocket 接口 ----

	virtual bool Shutdown(ESocketShutdownMode Mode) override;
	virtual bool Close() override;
	virtual bool Bind(const FInternetAddr& Addr) override;
	virtual bool Connect(const FInternetAddr& Addr) override;
	virtual bool Listen(int32 MaxBacklog) override;
	virtual bool WaitForPendingConnection(bool& bHasPendingConnection, const FTimespan& WaitTime) override;
	virtual bool HasPendingData(uint32& PendingDataSize) override;
	virtual class FSocket* Accept(const FString& InSocketDescription) override;
	virtual class FSocket* Accept(FInternetAddr& OutAddr, const FString& InSocketDescription) override;
	virtual bool SendTo(const uint8* Data, int32 Count, int32& BytesSent, const FInternetAddr& Destination) override;
	virtual bool RecvFrom(uint8* Data, int32 BufferSize, int32& BytesRead, FInternetAddr& Source, ESocketReceiveFlags::Type Flags = ESocketReceiveFlags::None) override;
	virtual bool Wait(ESocketWaitConditions::Type Condition, FTimespan WaitTime) override;
	virtual ESocketConnectionState GetConnectionState() override;
	virtual void GetAddress(FInternetAddr& OutAddr) override;
	virtual bool GetPeerAddress(FInternetAddr& OutAddr) override;
	virtual bool SetNonBlocking(bool bIsNonBlocking = true) override;
	virtual bool SetBroadcast(bool bAllowBroadcast = true) override;
	virtual bool SetNoDelay(bool bIsNoDelay = true) override;
	virtual bool JoinMulticastGroup(const FInternetAddr& GroupAddress) override;
	virtual bool JoinMulticastGroup(const FInternetAddr& GroupAddress, const FInternetAddr& InterfaceAddress) override;
	virtual bool LeaveMulticastGroup(const FInternetAddr& GroupAddress) override;
	virtual bool LeaveMulticastGroup(const FInternetAddr& GroupAddress, const FInternetAddr& InterfaceAddress) override;
	virtual bool SetMulticastLoopback(bool bLoopback) override;
	virtual bool SetMulticastTtl(uint8 TimeToLive) override;
	virtual bool SetMulticastInterface(const FInternetAddr& InterfaceAddress) override;
	virtual bool SetReuseAddr(bool bAllowReuse) override;
	virtual bool SetLinger(bool bShouldLinger, int32 Timeout) override;
	virtual bool SetRecvErr(bool bUseErrorQueue) override;
	virtual bool SetSendBufferSize(int32 Size, int32& OutNewSize) override;
	virtual bool SetReceiveBufferSize(int32 Size, int32& OutNewSize) override;
	virtual int32 GetPortNo() override;

	EWeaponRelaySocketState GetState() const { return State; }
	const FInternetAddr* GetRelayedAddress() const { return RelayedAddress.IsValid() ? RelayedAddress.Get() : nullptr; }

	/** TURN 分配失败且从未 Ready 时,透传底层 UDP 直连(优雅降级)。 */
	bool IsFallbackDirect() const { return bFallbackDirect; }

private:
	/** TURN 请求种类;认证请求在 438(stale nonce)后可按种类重建。 */
	enum class ERequestKind : uint8
	{
		Allocate,
		Refresh,
		Deallocate,          // Refresh LIFETIME=0:关闭时立即释放服务器上的 relay 端口
		CreatePermission,
		ChannelBind,
	};

	/** 在途事务:原始报文用于重传,种类+参数用于 nonce 续期后重建。 */
	struct FPendingRequest
	{
		ERequestKind Kind = ERequestKind::Allocate;
		TArray<uint8> Message;
		TSharedPtr<FInternetAddr> Peer;
		uint16 Channel = 0;
		double FirstSentTime = 0.0;
		double LastSentTime = 0.0;
		int32 Attempts = 0;
		bool bAuthenticated = false;
	};

	/** 经 TURN 中转的对端:各自的 channel 号与绑定状态。 */
	struct FRelayPeer
	{
		TSharedPtr<FInternetAddr> Addr;
		uint16 Channel = 0;
		bool bChannelBound = false;
		double LastBindTime = 0.0;
	};

	// TURN 事务(调用方须持有 StateLock)
	void SendRequest(ERequestKind Kind, const TSharedPtr<FInternetAddr>& Peer = nullptr, uint16 Channel = 0);
	bool BuildRequest(ERequestKind Kind, const FInternetAddr* Peer, uint16 Channel, TArray<uint8>& OutMsg, FString& OutTransactionKey) const;
	void HandleStunMessage(const uint8* Data, int32 Size);
	void HandleAllocateSuccess(const uint8* Data, int32 Size);
	void SendIndication(const uint8* Payload, int32 Count, const FInternetAddr& Peer);
	/** 候选测速 ping(WeaponConnectProbe)就地回 pong 并返回 true,不交给 NetDriver。 */
	bool EchoConnectProbe(const uint8* Data, int32 Size, const FInternetAddr& From);
	/** 所有发往 TURN 服务器的报文都经此发送,并记录时间供保活判断。 */
	bool SendToTurn(const uint8* Data, int32 Count);
	bool ExtractDataIndication(const uint8* Data, int32 Size, uint8* OutData, int32 BufferSize, int32& OutBytes, FInternetAddr& OutPeer);

	// peer / permission 管理(调用方须持有 StateLock)
	FRelayPeer& EnsurePeer(const FInternetAddr& PeerAddr);
	void EnsurePermission(const FInternetAddr& PeerAddr);
	bool IsFromTurnServer(const FInternetAddr& From) const;

	/** 统一失败入口:从未 Ready 时同时置降级标志。 */
	void EnterFailedState();

	/**
	 * 真正发起 Allocate 事务(发首个未认证请求)。
	 * 延迟到 Bind 成功后调用:若在 CreateSocketForProtocol 里立即 SendTo,
	 * OS 会给未绑定 socket 隐含分配随机端口,后续 InitBase 的 Bind(0.0.0.0:0)
	 * 会冲突(WSAEACCES)。Bind 后再 Allocate 则底层端口已固定,无冲突。
	 */
	void BeginAllocateTransaction();

	/**
	 * 解析 TURN 服务器地址。InTurnServerAddr 形如 host[:port]
	 * (host 可为 IP 或域名,端口缺省 3478),输出可直接 SendTo 的地址。
	 */
	bool ResolveTurnServerAddress(const FString& InTurnServerAddr, TSharedPtr<FInternetAddr>& OutAddr) const;

	// IpNetDriver 可能在接收线程调 RecvFrom、在 Game 线程调 SendTo/Tick
	mutable FCriticalSection StateLock;

	// 底层 socket
	FSocket* Underlying = nullptr;
	EWeaponRelaySocketState State = EWeaponRelaySocketState::Idle;

	// TURN 服务器地址(Allocate 时解析)
	TSharedPtr<FInternetAddr> TurnServerAddr;
	FString Username;
	FString Password;

	// 分配结果
	TSharedPtr<FInternetAddr> RelayedAddress;
	double LastAllocationRefreshTime = 0.0;
	// 最近一次发往 TURN 服务器的时间(NAT 保活依据)
	double LastTurnSendTime = 0.0;

	// TURN 认证往返:401/438 响应携带的 NONCE/REALM
	FString Nonce;
	FString Realm;

	// 在途事务:transaction id(hex) -> 请求
	TMap<FString, FPendingRequest> PendingRequests;

	// 经 TURN 中转的对端:peer key(ip:port) -> 状态;channel -> peer key
	TMap<FString, FRelayPeer> RelayPeers;
	TMap<uint16, FString> ChannelToPeer;
	uint16 NextChannel = 0x4000;

	// permission(按 IP,端口无关):ip key -> 上次安装时间(0 = 待 Allocate 完成后发送)
	TMap<FString, double> Permissions;
	TMap<FString, TSharedPtr<FInternetAddr>> PermissionAddrs;

	// 直连对端(源地址不是 TURN 服务器的入站 UDP):peer key 集合,回包走底层直发
	TSet<FString> DirectPeers;

	// 优雅降级:Allocate 失败且从未 Ready 时置位,此后所有 I/O 透传底层 UDP
	bool bFallbackDirect = false;
	bool bEverReady = false;

	// 底层 UDP socket 是否已绑定(Allocate 延迟到 Bind 后发起的依据)
	bool bUnderlyingBound = false;

	bool bNonBlocking = true;
};
