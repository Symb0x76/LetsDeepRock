// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * 连接前候选测速的线格式(客户端 ping,Host 的 relay socket 原样回 pong)。
 *
 *   [0..3]  魔数 'L','D','R','P'
 *   [4]     类型:0 = ping,1 = pong
 *   [5..7]  保留(0)
 *   [8..15] 序号(客户端用来区分候选与轮次)
 *   [16..23] 客户端发送时刻(FPlatformTime::Seconds 的 double 位模式),pong 原样带回
 *
 * 固定 24 字节 + 魔数,UE 握手包不会恰好同长同前缀;Host 识别后就地回复,不交给 NetDriver。
 */
namespace WeaponConnectProbe
{
	constexpr int32 PacketSize = 24;
	constexpr uint8 TypePing = 0;
	constexpr uint8 TypePong = 1;

	inline bool IsProbe(const uint8* Data, int32 Size, uint8 Type)
	{
		return Size == PacketSize && Data[0] == 'L' && Data[1] == 'D' && Data[2] == 'R' && Data[3] == 'P' && Data[4] == Type;
	}

	inline void Write(uint8* Out, uint8 Type, uint64 Sequence, double SentTime)
	{
		FMemory::Memzero(Out, PacketSize);
		Out[0] = 'L'; Out[1] = 'D'; Out[2] = 'R'; Out[3] = 'P';
		Out[4] = Type;
		FMemory::Memcpy(Out + 8, &Sequence, sizeof(Sequence));
		FMemory::Memcpy(Out + 16, &SentTime, sizeof(SentTime));
	}

	inline void Read(const uint8* Data, uint64& OutSequence, double& OutSentTime)
	{
		FMemory::Memcpy(&OutSequence, Data + 8, sizeof(OutSequence));
		FMemory::Memcpy(&OutSentTime, Data + 16, sizeof(OutSentTime));
	}
}
