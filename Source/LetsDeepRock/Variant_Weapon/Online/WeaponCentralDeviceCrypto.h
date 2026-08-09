// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"

struct FCryptoKeyPair;

/**
 * 设备身份加密封装:Ed25519 密钥生成/签名/公钥编码。
 *
 * 基于 UE 5.8 PlatformCrypto 插件的 OpenSSL 后端(FSignerEd25519 /
 * GenerateEd25519),与现有 SHA-256(IPlatformCrypto::CalcSHA256)同族,
 * 无新增第三方依赖。
 */
class FWeaponCentralDeviceCrypto
{
public:
	/** 生成一对新 Ed25519 密钥;失败返回 false。 */
	static bool GenerateKeyPair(TArray<uint8>& OutPrivateKey, TArray<uint8>& OutPublicKey);

	/**
	 * 用私钥签名消息。
	 * @param PrivateKey 32 字节 Ed25519 私钥
	 * @param Message    要签名的字节(如 nonce 的 UTF-8)
	 * @param OutSignature 输出 64 字节签名
	 */
	static bool Sign(
		const TArray<uint8>& PrivateKey,
		const TArrayView<const uint8> Message,
		TArray<uint8>& OutSignature);

	/** 把 32 字节公钥编码成 base64url(Go 侧 /v1/devices/challenge 的 publicKey 字段)。 */
	static FString EncodePublicKeyBase64Url(const TArray<uint8>& PublicKey);

	/** base64url 无 padding 编码(Go 侧 RawURLEncoding 兼容)。 */
	static FString EncodeBase64UrlNoPad(const TArray<uint8>& Data);

	/** 校验签名(本地自检/调试用;线上由 Go Coordinator 校验)。 */
	static bool Verify(
		const TArray<uint8>& PublicKey,
		const TArrayView<const uint8> Message,
		const TArrayView<const uint8> Signature);
};
