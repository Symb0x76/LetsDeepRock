// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

class FSocket;
class FInternetAddr;
class FWeaponRelaySocket;

/**
 * 连接前并行测速:同时向全部候选发 WeaponConnectProbe ping,收集 RTT。
 *
 * - direct 候选:一个临时 UDP socket 直发
 * - relay 候选:一个临时 FWeaponRelaySocket(独立 TURN allocation)经 TURN 发往 Host 的 relayed 地址
 * Host 的 relay socket 就地回 pong。全部应答或超出时间窗即结束;调用方按 RTT 排序后再逐个 travel。
 * 不改变 NetDriver:测速 socket 与之后 travel 用的 socket 无关,结束即释放(含 TURN 端口)。
 */
class FWeaponCandidateProber
{
public:
	struct FTarget
	{
		int32 QueueIndex = INDEX_NONE;
		TSharedPtr<FInternetAddr> Addr;
		bool bRelay = false;
		double RttSeconds = -1.0;   // <0 表示未应答
	};

	~FWeaponCandidateProber();

	void AddDirectTarget(int32 QueueIndex, const FString& Address, int32 Port);
	void AddRelayTarget(int32 QueueIndex, const FString& Address, int32 Port,
		const FString& TurnServerAddr, const FString& Username, const FString& Password);

	/** 打开 socket 并开始测速;没有任何可测目标时返回 false。 */
	bool Start(double InWindowSeconds);

	/** 发送/接收一轮;全部应答或时间窗结束时返回 true。 */
	bool Tick();

	const TArray<FTarget>& GetTargets() const { return Targets; }

private:
	void SendPings(double Now);
	void Drain(FSocket& Socket);
	TSharedPtr<FInternetAddr> MakeAddr(const FString& Address, int32 Port) const;

	TArray<FTarget> Targets;
	FSocket* DirectSocket = nullptr;
	FWeaponRelaySocket* RelaySocket = nullptr;
	FString TurnAddr;
	FString TurnUser;
	FString TurnPass;
	double StartTime = 0.0;
	double LastPingTime = 0.0;
	double WindowSeconds = 2.0;
};
