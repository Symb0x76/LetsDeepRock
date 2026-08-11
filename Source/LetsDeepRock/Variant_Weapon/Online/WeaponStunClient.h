// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

/**
 * 最小 STUN Binding 客户端(RFC 5389)。
 *
 * 用途:Host 发布 endpoint 前,向 STUN 服务器发起 UDP Binding 请求,
 * 从 XOR-MAPPED-ADDRESS 拿到本机在公网的反射地址(server-reflexive),
 * 作为跨 NAT 直连候选。
 *
 * 刻意不经过 libcurl/HTTP:HTTP 会走系统/环境代理,拿到的是代理出口 IP
 * 而非本机真实公网出口(本项目 DefaultEngine.ini 已显式 HttpNoProxy,
 * 但 UDP 走 socket 子系统与 HTTP 通道完全隔离,天然免疫代理)。
 */
class LETSDEEPROCK_API FWeaponStunClient
{
public:
	FWeaponStunClient() = default;

	/** 解析 stun:host:port / host:port 形式的 STUN 服务器地址。 */
	static bool ParseServerUrl(const FString& ServerUrl, FString& OutHost, int32& OutPort);

	/**
	 * 同步向 STUN 服务器发起 Binding 请求并等待响应。
	 * @param ServerHost   STUN 服务器主机(IP 或域名)
	 * @param ServerPort   STUN 端口(默认 3478)
	 * @param OutMappedAddress 返回的反射地址(公网 IP)
	 * @param OutMappedPort    返回的反射端口
	 * @param TimeoutSeconds  等待响应的超时
	 * @return 收到有效 Binding 响应且解析出映射地址时为 true
	 */
	bool QueryReflexiveAddress(
		const FString& ServerHost,
		int32 ServerPort,
		FString& OutMappedAddress,
		int32& OutMappedPort,
		float TimeoutSeconds = 3.0f);

private:
	bool SendBindingRequest(FSocket* Socket, const FInternetAddr& ServerAddr);
	bool ParseBindingResponse(const uint8* Data, int32 Size, FString& OutAddress, int32& OutPort) const;
};
