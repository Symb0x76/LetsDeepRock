// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponStunClient.h"

#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"
#include "HAL/PlatformProcess.h"
#include "HAL/PlatformTime.h"

DEFINE_LOG_CATEGORY_STATIC(LogWeaponStunClient, Log, All);

namespace WeaponStun
{
	constexpr uint16 HeaderSize = 20;

	// RFC 5389 message types
	constexpr uint16 BindingRequest = 0x0001;
	constexpr uint16 BindingSuccessResponse = 0x0101;
	constexpr uint16 BindingErrorResponse = 0x0111;

	// Magic cookie per RFC 5389
	constexpr uint32 MagicCookie = 0x2112A442;

	// Attribute types
	constexpr uint16 AttrMappedAddress = 0x0001;
	constexpr uint16 AttrXorMappedAddress = 0x0020;
	constexpr uint16 AttrErrorCode = 0x0009;

	// Family codes
	constexpr uint8 FamilyIPv4 = 0x01;
	constexpr uint8 FamilyIPv6 = 0x02;

	// Default port when the STUN URL omits it
	constexpr int32 DefaultPort = 3478;
}

bool FWeaponStunClient::ParseServerUrl(const FString& ServerUrl, FString& OutHost, int32& OutPort)
{
	OutHost.Reset();
	OutPort = WeaponStun::DefaultPort;

	FString Work = ServerUrl.TrimStartAndEnd();
	if (Work.StartsWith(TEXT("stun:"), ESearchCase::IgnoreCase))
	{
		Work = Work.RightChop(5);
	}
	if (Work.IsEmpty())
	{
		return false;
	}

	// Host may be [IPv6]:port or host:port; bare host also accepted.
	if (Work.StartsWith(TEXT("[")))
	{
		const int32 CloseBracket = Work.Find(TEXT("]"));
		if (CloseBracket == INDEX_NONE)
		{
			return false;
		}
		OutHost = Work.Mid(1, CloseBracket - 1);
		const FString Remainder = Work.RightChop(CloseBracket + 1);
		if (Remainder.StartsWith(TEXT(":")))
		{
			OutPort = FCString::Atoi(*Remainder.RightChop(1));
		}
	}
	else
	{
		const int32 Colon = Work.Find(TEXT(":"));
		if (Colon == INDEX_NONE)
		{
			OutHost = Work;
		}
		else
		{
			OutHost = Work.Left(Colon);
			OutPort = FCString::Atoi(*Work.RightChop(Colon + 1));
		}
	}

	if (OutHost.IsEmpty() || OutPort <= 0 || OutPort > 65535)
	{
		return false;
	}
	return true;
}

bool FWeaponStunClient::QueryReflexiveAddress(
	const FString& ServerHost,
	const int32 ServerPort,
	FString& OutMappedAddress,
	int32& OutMappedPort,
	const float TimeoutSeconds)
{
	OutMappedAddress.Reset();
	OutMappedPort = 0;

	ISocketSubsystem* SocketSubsystem = ISocketSubsystem::Get(PLATFORM_SOCKETSUBSYSTEM);
	if (!SocketSubsystem)
	{
		UE_LOG(LogWeaponStunClient, Warning, TEXT("QueryReflexiveAddress: socket subsystem unavailable"));
		return false;
	}

	// GetAddressInfo 支持域名与 IP,端口经 ServiceName 传入并自动填入返回地址。
	const FString Service = FString::Printf(TEXT("%d"), ServerPort);
	const FAddressInfoResult Result = SocketSubsystem->GetAddressInfo(*ServerHost, *Service, EAddressInfoFlags::Default, NAME_None);
	if (Result.ReturnCode != SE_NO_ERROR || Result.Results.Num() == 0)
	{
		UE_LOG(LogWeaponStunClient, Warning, TEXT("QueryReflexiveAddress: cannot resolve %s"), *ServerHost);
		return false;
	}
	const TSharedRef<FInternetAddr> ServerAddr = Result.Results[0].Address->Clone();

	FSocket* Socket = SocketSubsystem->CreateSocket(NAME_DGram, TEXT("WeaponStunClient"));
	if (!Socket)
	{
		UE_LOG(LogWeaponStunClient, Warning, TEXT("QueryReflexiveAddress: failed to create UDP socket"));
		return false;
	}
	Socket->SetNonBlocking(false);

	const bool bSent = SendBindingRequest(Socket, ServerAddr.Get());
	if (!bSent)
	{
		SocketSubsystem->DestroySocket(Socket);
		return false;
	}

	TArray<uint8> Buffer;
	Buffer.SetNumUninitialized(2048);
	const TSharedRef<FInternetAddr> FromAddr = SocketSubsystem->CreateInternetAddr();

	const double Deadline = FPlatformTime::Seconds() + FMath::Max(0.1f, TimeoutSeconds);
	bool bGotResponse = false;
	while (FPlatformTime::Seconds() < Deadline)
	{
		int32 BytesRead = 0;
		if (Socket->RecvFrom(Buffer.GetData(), Buffer.Num(), BytesRead, FromAddr.Get(), ESocketReceiveFlags::None))
		{
			bGotResponse = true;
			if (ParseBindingResponse(Buffer.GetData(), BytesRead, OutMappedAddress, OutMappedPort))
			{
				UE_LOG(LogWeaponStunClient, Log, TEXT("STUN reflexive address: %s:%d (via %s)"),
					*OutMappedAddress, OutMappedPort, *ServerHost);
				SocketSubsystem->DestroySocket(Socket);
				return true;
			}
			UE_LOG(LogWeaponStunClient, Warning, TEXT("STUN response from %s could not be parsed"), *ServerHost);
			break;
		}
		FPlatformProcess::Sleep(0.02f);
	}

	if (!bGotResponse)
	{
		UE_LOG(LogWeaponStunClient, Warning, TEXT("STUN request to %s:%d timed out"), *ServerHost, ServerPort);
	}
	SocketSubsystem->DestroySocket(Socket);
	return false;
}

bool FWeaponStunClient::SendBindingRequest(FSocket* Socket, const FInternetAddr& ServerAddr)
{
	TArray<uint8> Msg;
	Msg.SetNumUninitialized(WeaponStun::HeaderSize);

	Msg[0] = static_cast<uint8>(WeaponStun::BindingRequest >> 8);
	Msg[1] = static_cast<uint8>(WeaponStun::BindingRequest & 0xFF);
	Msg[2] = 0; // message length (no attributes)
	Msg[3] = 0;
	Msg[4] = static_cast<uint8>(WeaponStun::MagicCookie >> 24);
	Msg[5] = static_cast<uint8>(WeaponStun::MagicCookie >> 16);
	Msg[6] = static_cast<uint8>(WeaponStun::MagicCookie >> 8);
	Msg[7] = static_cast<uint8>(WeaponStun::MagicCookie & 0xFF);
	for (int32 i = 8; i < 20; ++i)
	{
		Msg[i] = static_cast<uint8>(FMath::Rand() & 0xFF);
	}

	int32 BytesSent = 0;
	if (!Socket->SendTo(Msg.GetData(), Msg.Num(), BytesSent, ServerAddr))
	{
		UE_LOG(LogWeaponStunClient, Warning, TEXT("STUN Binding request send failed"));
		return false;
	}
	return BytesSent == Msg.Num();
}

bool FWeaponStunClient::ParseBindingResponse(const uint8* Data, const int32 Size, FString& OutAddress, int32& OutPort) const
{
	if (Size < WeaponStun::HeaderSize)
	{
		return false;
	}

	const uint16 MessageType = static_cast<uint16>((Data[0] << 8) | Data[1]);
	const uint16 Length = static_cast<uint16>((Data[2] << 8) | Data[3]);
	const uint32 Cookie =
		(static_cast<uint32>(Data[4]) << 24) |
		(static_cast<uint32>(Data[5]) << 16) |
		(static_cast<uint32>(Data[6]) << 8) |
		static_cast<uint32>(Data[7]);

	if (Cookie != WeaponStun::MagicCookie)
	{
		return false;
	}
	if (MessageType == WeaponStun::BindingErrorResponse)
	{
		UE_LOG(LogWeaponStunClient, Warning, TEXT("STUN Binding error response received"));
		return false;
	}
	if (MessageType != WeaponStun::BindingSuccessResponse)
	{
		return false;
	}

	// XOR-MAPPED-ADDRESS (0x0020) is mandatory in RFC 5389 responses.
	// XOR with the cookie for IPv4, or cookie + transaction id for IPv6.
	int32 Offset = WeaponStun::HeaderSize;
	const int32 End = FMath::Min(Size, WeaponStun::HeaderSize + static_cast<int32>(Length));
	while (Offset + 4 <= End)
	{
		const uint16 AttrType = static_cast<uint16>((Data[Offset] << 8) | Data[Offset + 1]);
		const uint16 AttrLen = static_cast<uint16>((Data[Offset + 2] << 8) | Data[Offset + 3]);
		const int32 AttrEnd = Offset + 4 + AttrLen;
		if (AttrEnd > End)
		{
			break;
		}

		if (AttrType == WeaponStun::AttrXorMappedAddress && AttrLen >= 8)
		{
			if (const uint8 Family = Data[Offset + 4 + 1]; Family == WeaponStun::FamilyIPv4 && AttrLen >= 8)
			{
				const uint16 XorPort = static_cast<uint16>((Data[Offset + 4 + 2] << 8) | Data[Offset + 4 + 3]);
				OutPort = XorPort ^ static_cast<uint16>((WeaponStun::MagicCookie >> 16) & 0xFFFF);
				OutAddress = FString::Printf(
					TEXT("%d.%d.%d.%d"),
					Data[Offset + 4 + 4] ^ Data[4],
					Data[Offset + 4 + 5] ^ Data[5],
					Data[Offset + 4 + 6] ^ Data[6],
					Data[Offset + 4 + 7] ^ Data[7]);
				return !OutAddress.IsEmpty() && OutPort > 0;
			}
			else if (Family == WeaponStun::FamilyIPv6 && AttrLen >= 20)
			{
				// RFC 5389: IPv6 XOR uses the transaction id (bytes 8..19)
				TArray<uint8> AddrBytes;
				AddrBytes.SetNumUninitialized(16);
				for (int32 i = 0; i < 16; ++i)
				{
					AddrBytes[i] = Data[Offset + 4 + 4 + i] ^ Data[8 + i];
				}
				OutPort = static_cast<uint16>(
					((Data[Offset + 4 + 2] << 8) | Data[Offset + 4 + 3]) ^
					((WeaponStun::MagicCookie >> 16) & 0xFFFF));
				OutAddress = FString::Printf(
					TEXT("%x:%x:%x:%x:%x:%x:%x:%x"),
					(AddrBytes[0] << 8) | AddrBytes[1],
					(AddrBytes[2] << 8) | AddrBytes[3],
					(AddrBytes[4] << 8) | AddrBytes[5],
					(AddrBytes[6] << 8) | AddrBytes[7],
					(AddrBytes[8] << 8) | AddrBytes[9],
					(AddrBytes[10] << 8) | AddrBytes[11],
					(AddrBytes[12] << 8) | AddrBytes[13],
					(AddrBytes[14] << 8) | AddrBytes[15]);
				return !OutAddress.IsEmpty() && OutPort > 0;
			}
		}

		// Attribute length is already 4-byte padded on the wire; skip value + padding.
		Offset = AttrEnd + ((-AttrLen) & 3);
	}

	return false;
}
