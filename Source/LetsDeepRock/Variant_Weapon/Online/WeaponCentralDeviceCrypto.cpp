// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponCentralDeviceCrypto.h"

#include "Misc/Base64.h"

// OpenSSL 的 `typedef struct ui_st UI` 与 Slate 的 `namespace UI` 冲突。
// 只在 OpenSSL include 前把 UI 改名为无冲突标识符;本文件不调用任何 UI API。
#define UI OpenSSL_UI_Type_Unused
THIRD_PARTY_INCLUDES_START
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/err.h>
THIRD_PARTY_INCLUDES_END
#undef UI

namespace
{
	constexpr int32 Ed25519KeyBytes = 32;
	constexpr int32 Ed25519SignatureBytes = 64;

	bool OpenSslInitKey(EVP_PKEY** OutKey, const TArray<uint8>& RawKey, bool bPrivate)
	{
		const unsigned char* Data = RawKey.GetData();
		EVP_PKEY* Key = bPrivate
			? EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, Data, RawKey.Num())
			: EVP_PKEY_new_raw_public_key(EVP_PKEY_ED25519, nullptr, Data, RawKey.Num());
		if (!Key)
		{
			return false;
		}
		*OutKey = Key;
		return true;
	}
}

bool FWeaponCentralDeviceCrypto::GenerateKeyPair(
	TArray<uint8>& OutPrivateKey,
	TArray<uint8>& OutPublicKey)
{
	// OpenSSL Ed25519: 私钥 32 字节(与 Go crypto/ed25519 一致),公钥 32 字节。
	OutPrivateKey.SetNumUninitialized(Ed25519KeyBytes);
	OutPublicKey.SetNumUninitialized(Ed25519KeyBytes);

	EVP_PKEY* Key;
	{
		// 随机 32 字节种子 → raw 私钥
		unsigned char* Seed = OutPrivateKey.GetData();
		if (RAND_bytes(Seed, Ed25519KeyBytes) != 1)
		{
			OutPrivateKey.Reset();
			OutPublicKey.Reset();
			return false;
		}

		Key = EVP_PKEY_new_raw_private_key(EVP_PKEY_ED25519, nullptr, Seed, Ed25519KeyBytes);
		if (!Key)
		{
			OutPrivateKey.Reset();
			OutPublicKey.Reset();
			return false;
		}
	}

	size_t PublicLen = Ed25519KeyBytes;
	if (EVP_PKEY_get_raw_public_key(Key, OutPublicKey.GetData(), &PublicLen) != 1)
	{
		EVP_PKEY_free(Key);
		OutPrivateKey.Reset();
		OutPublicKey.Reset();
		return false;
	}
	EVP_PKEY_free(Key);
	return true;
}

bool FWeaponCentralDeviceCrypto::Sign(
	const TArray<uint8>& PrivateKey,
	const TArrayView<const uint8> Message,
	TArray<uint8>& OutSignature)
{
	if (PrivateKey.Num() != Ed25519KeyBytes)
	{
		return false;
	}

	EVP_PKEY* Key = nullptr;
	if (!OpenSslInitKey(&Key, PrivateKey, /*bPrivate=*/true))
	{
		return false;
	}

	EVP_MD_CTX* Ctx = EVP_MD_CTX_new();
	if (!Ctx)
	{
		EVP_PKEY_free(Key);
		return false;
	}

	bool bOk = false;
	if (EVP_DigestSignInit(Ctx, nullptr, nullptr, nullptr, Key) == 1)
	{
		OutSignature.SetNumUninitialized(Ed25519SignatureBytes);
		size_t SigLen = OutSignature.Num();
		if (EVP_DigestSign(Ctx, OutSignature.GetData(), &SigLen, Message.GetData(), Message.Num()) == 1 &&
			SigLen == Ed25519SignatureBytes)
		{
			bOk = true;
		}
		else
		{
			OutSignature.Reset();
		}
	}

	EVP_MD_CTX_free(Ctx);
	EVP_PKEY_free(Key);
	return bOk;
}

FString FWeaponCentralDeviceCrypto::EncodePublicKeyBase64Url(const TArray<uint8>& PublicKey)
{
	return EncodeBase64UrlNoPad(PublicKey);
}

FString FWeaponCentralDeviceCrypto::EncodeBase64UrlNoPad(const TArray<uint8>& Data)
{
	// UE 的 FBase64::Encode(UrlSafe) 会带 '=' padding(1 或 2 个,取决于长度);
	// Go 侧用 base64.RawURLEncoding(严格无 padding),这里去掉全部尾部 '=' 对齐。
	FString Encoded = FBase64::Encode(Data, EBase64Mode::UrlSafe);
	while (Encoded.EndsWith(TEXT("=")))
	{
		Encoded.LeftChopInline(1);
	}
	return Encoded;
}

bool FWeaponCentralDeviceCrypto::Verify(
	const TArray<uint8>& PublicKey,
	const TArrayView<const uint8> Message,
	const TArrayView<const uint8> Signature)
{
	if (PublicKey.Num() != Ed25519KeyBytes || Signature.Num() != Ed25519SignatureBytes)
	{
		return false;
	}

	EVP_PKEY* Key = nullptr;
	if (!OpenSslInitKey(&Key, PublicKey, /*bPrivate=*/false))
	{
		return false;
	}

	EVP_MD_CTX* Ctx = EVP_MD_CTX_new();
	if (!Ctx)
	{
		EVP_PKEY_free(Key);
		return false;
	}

	bool bOk = false;
	if (EVP_DigestVerifyInit(Ctx, nullptr, nullptr, nullptr, Key) == 1)
	{
		bOk = (EVP_DigestVerify(Ctx, Signature.GetData(), Signature.Num(), Message.GetData(), Message.Num()) == 1);
	}

	EVP_MD_CTX_free(Ctx);
	EVP_PKEY_free(Key);
	return bOk;
}
