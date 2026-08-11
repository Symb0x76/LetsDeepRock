// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Sockets.h"
#include "WeaponRelaySocket.generated.h"

class FWeaponRelaySocket;

/**
 * 状态码:Relay 路径的分配/绑定/数据面状态。
 */
UENUM()
enum class EWeaponRelaySocketState : uint8
{
	Idle,
	Allocating,          // Allocate 事务进行中
	Allocated,           // 已在 TURN 服务器上分配 relayed address
	Binding,             // ChannelBind 进行中
	Ready,               // ChannelData 数据面可用
	Failed,              // 不可恢复错误
	Closed,
};

/**
 * FWeaponRelaySocket 把 TURN 服务器当作 peer:它是一个"虚拟 FSocket"。
 *
 * UE 的 IpNetDriver/NetConnection 通过 FSocket 收发字节;本类把 FSocket
 * 接口映射到 TURN 数据面:
 *   - 底层持有真正的 UDP FSocket(直连 TURN 服务器)
 *   - SendTo(Data, Dest)  -> 封装成 ChannelData(若已绑定)或 Send Indication 发给服务器
 *   - RecvFrom 解出服务器转发的 peer 数据
 *   - GetAddress 返回虚拟的 relayed address(对端看到的"我")
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

	/** 为对端地址建立 ChannelBind(在收到对端首个数据后调用)。 */
	bool BindChannel(const FInternetAddr& PeerAddr);

	/** 由外部轮询驱动的 TURN 事务推进(每帧调用,处理重传/超时)。 */
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
	// TURN 事务
	bool SendStunTransaction(const TArray<uint8>& Message);
	void HandleControlPacket(const TArray<uint8>& Data, const FInternetAddr& From);
	void OnAllocateResponse(const uint8* Data, int32 Size);
	void OnChannelBindResponse(const uint8* Data, int32 Size);
	void OnDataIndication(const uint8* Data, int32 Size);
	void SendAuthenticatedAllocate();
	/** 统一失败入口:从未 Ready 时同时置降级标志。 */
	void EnterFailedState();

	// 底层 socket
	FSocket* Underlying = nullptr;
	EWeaponRelaySocketState State = EWeaponRelaySocketState::Idle;

	// TURN 服务器地址(Allocate 时解析)
	TSharedPtr<FInternetAddr> TurnServerAddr;
	FString Username;
	FString Password;

	// 分配结果
	TSharedPtr<FInternetAddr> RelayedAddress;

	// ChannelBind 状态:channel number -> peer addr
	uint16 ChannelNumber = 0;
	TSharedPtr<FInternetAddr> BoundPeerAddr;
	bool bChannelBound = false;

	// 接收缓冲:底层 socket 读出的原始数据(含 STUN/ChannelData 头)
	TArray<uint8> ReadBuffer;
	int32 ReadBufferOffset = 0;

	// TURN 认证往返:401 响应携带的 NONCE/REALM
	FString Nonce;
	FString Realm;
	bool bAllocateAuthenticated = false;

	// 事务超时
	double TransactionStartTime = 0.0;
	bool bTransactionPending = false;

	// 优雅降级:Allocate 失败且从未 Ready 时置位,此后所有 I/O 透传底层 UDP
	bool bFallbackDirect = false;

	bool bNonBlocking = true;
};
