// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponRelaySocket.h"
#include "Sockets.h"
#include "SocketSubsystem.h"
#include "IPAddress.h"

// OpenSSL 的 `typedef struct ui_st UI` 与 UE 的 `namespace UI` 冲突。
// 只在 OpenSSL include 前把 UI 改名为无冲突标识符;本文件不调用任何 UI API。
#define UI OpenSSL_UI_Type_Unused
THIRD_PARTY_INCLUDES_START
#include <openssl/hmac.h>
#include <openssl/md5.h>
THIRD_PARTY_INCLUDES_END
#undef UI

DECLARE_LOG_CATEGORY_EXTERN(LogWeaponRelay, Log, All);
DEFINE_LOG_CATEGORY(LogWeaponRelay);

// ---- STUN constants (RFC 5389 / 5766) ----

namespace WeaponRelayStun
{
	constexpr uint32 MagicCookie32 = 0x2112A442;

	// message types
	constexpr uint16 BindingRequest        = 0x0001;
	constexpr uint16 AllocateRequest       = 0x0003;
	constexpr uint16 AllocateSuccess       = 0x0103;
	constexpr uint16 AllocateError         = 0x0113;
	constexpr uint16 RefreshRequest        = 0x0004;
	constexpr uint16 CreatePermissionReq   = 0x0008;
	constexpr uint16 CreatePermissionSucc  = 0x0108;
	constexpr uint16 CreatePermissionErr   = 0x0118;
	constexpr uint16 ChannelBindRequest    = 0x0009;
	constexpr uint16 ChannelBindSuccess    = 0x0109;
	constexpr uint16 ChannelBindError      = 0x0119;
	constexpr uint16 SendIndication        = 0x0016;
	constexpr uint16 DataIndication        = 0x0017;

	// attribute types
	constexpr uint16 AttrMappedAddress       = 0x0001;
	constexpr uint16 AttrUsername            = 0x0006;
	constexpr uint16 AttrMessageIntegrity    = 0x0008;
	constexpr uint16 AttrErrorCode           = 0x0009;
	constexpr uint16 AttrChannelNumber       = 0x000C;
	constexpr uint16 AttrLifetime            = 0x000D;
	constexpr uint16 AttrXorPeerAddress      = 0x0012;
	constexpr uint16 AttrData                = 0x0013;
	constexpr uint16 AttrRealm               = 0x0014;
	constexpr uint16 AttrNonce               = 0x0015;
	constexpr uint16 AttrXorRelayedAddress   = 0x0016;
	constexpr uint16 AttrRequestedTransport  = 0x0019;

	constexpr uint32 HeaderSize = 20;

	// ChannelData first byte must have the two top bits set (0b01)
	constexpr uint16 ChannelDataMask    = 0xC000;
	constexpr uint16 ChannelDataMarker  = 0x4000;

	// channel numbers allocated by clients live in 0x4000-0x7FFF
	constexpr uint16 ChannelBase = 0x4000;
}

namespace
{
	uint16 ReadU16(const uint8* P) { return static_cast<uint16>((P[0] << 8) | P[1]); }
	void WriteU16(TArray<uint8>& Out, uint16 V) { Out.Add(static_cast<uint8>(V >> 8)); Out.Add(static_cast<uint8>(V & 0xFF)); }
	void WriteU32(TArray<uint8>& Out, uint32 V)
	{
		Out.Add(static_cast<uint8>(V >> 24)); Out.Add(static_cast<uint8>((V >> 16) & 0xFF));
		Out.Add(static_cast<uint8>((V >> 8) & 0xFF)); Out.Add(static_cast<uint8>(V & 0xFF));
	}

	// XOR-mapped address encoding per RFC 5389 §15.2 (IPv4)
	void EncodeXorAddr(TArray<uint8>& Out, uint16 AttrType, const FInternetAddr& Addr, const uint8* TransactionId)
	{
		WriteU16(Out, AttrType);
		WriteU16(Out, 8);
		Out.Add(0); Out.Add(0x01); // family: IPv4
		uint32 Ip = 0; Addr.GetIp(Ip);
		uint8 B0 = static_cast<uint8>(Ip), B1 = static_cast<uint8>(Ip >> 8), B2 = static_cast<uint8>(Ip >> 16), B3 = static_cast<uint8>(Ip >> 24);
		uint8 C0 = static_cast<uint8>(WeaponRelayStun::MagicCookie32 >> 24), C1 = static_cast<uint8>(WeaponRelayStun::MagicCookie32 >> 16), C2 = static_cast<uint8>(WeaponRelayStun::MagicCookie32 >> 8), C3 = static_cast<uint8>(WeaponRelayStun::MagicCookie32);
		Out.Add(B0 ^ C0); Out.Add(B1 ^ C1); Out.Add(B2 ^ C2); Out.Add(B3 ^ C3);
		uint16 Port = Addr.GetPort();
		uint8 P0 = static_cast<uint8>(Port >> 8), P1 = static_cast<uint8>(Port);
		Out.Add(P0 ^ C0); Out.Add(P1 ^ C1);
	}

	void DecodeXorAddr(const uint8* AttrData, const uint8* TransactionId, FInternetAddr& OutAddr)
	{
		// attr: family(1) + port(2) + address(4) = 7 bytes after padding byte
		uint8 C0 = static_cast<uint8>(WeaponRelayStun::MagicCookie32 >> 24), C1 = static_cast<uint8>(WeaponRelayStun::MagicCookie32 >> 16);
		uint8 T0 = TransactionId[0], T1 = TransactionId[1];
		uint16 Port = (static_cast<uint16>(AttrData[1] ^ C0) << 8) | (AttrData[2] ^ C1);
		uint32 Ip = (static_cast<uint32>(AttrData[3] ^ C0) << 24)
			| (static_cast<uint32>(AttrData[4] ^ T0) << 16)
			| (static_cast<uint32>(AttrData[5] ^ T1) << 8)
			| (static_cast<uint32>(AttrData[6] ^ TransactionId[2]));
		OutAddr.SetIp(Ip);
		OutAddr.SetPort(Port);
	}

	// RFC 5389 §15.4: key = MD5(username ":" realm ":" password)
	TArray<uint8> MakeAuthKey(const FString& Username, const FString& Realm, const FString& Password)
	{
		const FString Combined = Username + TEXT(":") + Realm + TEXT(":") + Password;
		FTCHARToUTF8 Utf8(*Combined);
		TArray<uint8> Key;
		Key.SetNumUninitialized(16);
		MD5(reinterpret_cast<const uint8*>(Utf8.Get()), Utf8.Length(), Key.GetData());
		return Key;
	}

	// Append MESSAGE-INTEGRITY to a message and return the full message.
	void AppendMessageIntegrity(TArray<uint8>& Msg, const TArray<uint8>& Key)
	{
		// Length must include the MESSAGE-INTEGRITY attribute (4 header + 16 HMAC).
		const uint16 NewLen = static_cast<uint16>(Msg.Num() - WeaponRelayStun::HeaderSize + 20);
		Msg[2] = static_cast<uint8>(NewLen >> 8);
		Msg[3] = static_cast<uint8>(NewLen & 0xFF);

		WriteU16(Msg, WeaponRelayStun::AttrMessageIntegrity);
		WriteU16(Msg, 16);
		const int32 HmacOffset = Msg.Num();
		Msg.AddZeroed(16);

		uint32 HmacLen = 0;
		HMAC(EVP_sha1(), Key.GetData(), Key.Num(), Msg.GetData(), Msg.Num() - 16, Msg.GetData() + HmacOffset, &HmacLen);
		check(HmacLen == 16);
	}
}

FWeaponRelaySocket::FWeaponRelaySocket(FSocket* InUnderlyingSocket, const FString& InDescription)
	: FSocket(SOCKTYPE_Datagram, InDescription, FNetworkProtocolTypes::IPv4)
	, Underlying(InUnderlyingSocket)
{
}

FWeaponRelaySocket::~FWeaponRelaySocket()
{
	Close();
}

// ---- TURN 控制面 ----

bool FWeaponRelaySocket::Allocate(const FString& InTurnServerAddr, const FString& InUsername, const FString& InPassword)
{
	if (!Underlying)
	{
		return false;
	}

	ISocketSubsystem* Sockets = ISocketSubsystem::Get();
	TSharedPtr<FInternetAddr> Addr = Sockets->GetAddressFromString(InTurnServerAddr);
	if (!Addr.IsValid())
	{
		UE_LOG(LogWeaponRelay, Error, TEXT("Allocate: cannot resolve TURN server %s"), *InTurnServerAddr);
		return false;
	}

	TurnServerAddr = Addr;
	Username = InUsername;
	Password = InPassword;
	State = EWeaponRelaySocketState::Allocating;

	// First Allocate is unauthenticated; the server replies 401 with
	// NONCE/REALM, which we answer with an authenticated retry.
	TArray<uint8> Msg;
	WriteU16(Msg, WeaponRelayStun::AllocateRequest);
	WriteU16(Msg, 0); // length patched below
	WriteU32(Msg, WeaponRelayStun::MagicCookie32);
	for (int32 i = 0; i < 12; ++i)
	{
		Msg.Add(FMath::Rand() & 0xFF);
	}
	// REQUESTED-TRANSPORT: UDP
	WriteU16(Msg, WeaponRelayStun::AttrRequestedTransport);
	WriteU16(Msg, 4);
	Msg.Add(17); Msg.Add(0); Msg.Add(0); Msg.Add(0);
	const uint16 Len = static_cast<uint16>(Msg.Num() - WeaponRelayStun::HeaderSize);
	Msg[2] = static_cast<uint8>(Len >> 8);
	Msg[3] = static_cast<uint8>(Len & 0xFF);

	TransactionStartTime = FPlatformTime::Seconds();
	bTransactionPending = true;
	return SendStunTransaction(Msg);
}

bool FWeaponRelaySocket::BindChannel(const FInternetAddr& PeerAddr)
{
	if (State != EWeaponRelaySocketState::Allocated)
	{
		UE_LOG(LogWeaponRelay, Warning, TEXT("BindChannel: not allocated (state %d)"), static_cast<int32>(State));
		return false;
	}

	TArray<uint8> Msg;
	WriteU16(Msg, WeaponRelayStun::ChannelBindRequest);
	WriteU16(Msg, 0);
	WriteU32(Msg, WeaponRelayStun::MagicCookie32);
	for (int32 i = 0; i < 12; ++i)
	{
		Msg.Add(FMath::Rand() & 0xFF);
	}
	// CHANNEL-NUMBER
	WriteU16(Msg, WeaponRelayStun::AttrChannelNumber);
	WriteU16(Msg, 4);
	WriteU16(Msg, WeaponRelayStun::ChannelBase);
	WriteU16(Msg, 0);
	// XOR-PEER-ADDRESS
	EncodeXorAddr(Msg, WeaponRelayStun::AttrXorPeerAddress, PeerAddr, Msg.GetData() + 8);
	// USERNAME / REALM / NONCE from the Allocate round-trip
	FTCHARToUTF8 UsernameUtf8(*Username);
	WriteU16(Msg, WeaponRelayStun::AttrUsername);
	WriteU16(Msg, static_cast<uint16>(UsernameUtf8.Length()));
	for (int32 i = 0; i < UsernameUtf8.Length(); ++i)
	{
		Msg.Add(static_cast<uint8>(UsernameUtf8.Get()[i]));
	}
	FTCHARToUTF8 RealmUtf8(*Realm);
	WriteU16(Msg, WeaponRelayStun::AttrRealm);
	WriteU16(Msg, static_cast<uint16>(RealmUtf8.Length()));
	for (int32 i = 0; i < RealmUtf8.Length(); ++i)
	{
		Msg.Add(static_cast<uint8>(RealmUtf8.Get()[i]));
	}
	FTCHARToUTF8 NonceUtf8(*Nonce);
	WriteU16(Msg, WeaponRelayStun::AttrNonce);
	WriteU16(Msg, static_cast<uint16>(NonceUtf8.Length()));
	for (int32 i = 0; i < NonceUtf8.Length(); ++i)
	{
		Msg.Add(static_cast<uint8>(NonceUtf8.Get()[i]));
	}
	AppendMessageIntegrity(Msg, MakeAuthKey(Username, Realm, Password));
	const uint16 Len2 = static_cast<uint16>(Msg.Num() - WeaponRelayStun::HeaderSize);
	Msg[2] = static_cast<uint8>(Len2 >> 8);
	Msg[3] = static_cast<uint8>(Len2 & 0xFF);

	BoundPeerAddr = PeerAddr.Clone();
	State = EWeaponRelaySocketState::Binding;
	TransactionStartTime = FPlatformTime::Seconds();
	bTransactionPending = true;
	return SendStunTransaction(Msg);
}

void FWeaponRelaySocket::Tick()
{
	if (!bTransactionPending || !Underlying)
	{
		return;
	}
	// If the transaction times out, fail the state machine.
	const double Elapsed = FPlatformTime::Seconds() - TransactionStartTime;
	if (Elapsed > 5.0)
	{
		UE_LOG(LogWeaponRelay, Error, TEXT("TURN transaction timed out (state %d)"), static_cast<int32>(State));
		bTransactionPending = false;
		State = EWeaponRelaySocketState::Failed;
	}
}

bool FWeaponRelaySocket::HasPendingRelayData(uint32& PendingDataSize)
{
	if (!Underlying)
	{
		return false;
	}
	return Underlying->HasPendingData(PendingDataSize);
}

// ---- FSocket 接口 ----

bool FWeaponRelaySocket::Shutdown(ESocketShutdownMode Mode)
{
	return Underlying ? Underlying->Shutdown(Mode) : false;
}

bool FWeaponRelaySocket::Close()
{
	if (Underlying)
	{
		Underlying->Close();
		delete Underlying;
		Underlying = nullptr;
	}
	State = EWeaponRelaySocketState::Closed;
	return true;
}

bool FWeaponRelaySocket::Bind(const FInternetAddr& Addr)
{
	return Underlying ? Underlying->Bind(Addr) : false;
}

bool FWeaponRelaySocket::Connect(const FInternetAddr& Addr)
{
	// The relay socket is datagram-based and does not "connect" in the TCP
	// sense; nothing to do. Data flows through the TURN server.
	return true;
}

bool FWeaponRelaySocket::Listen(int32 MaxBacklog)
{
	return true; // datagram: no listen semantics
}

bool FWeaponRelaySocket::WaitForPendingConnection(bool& bHasPendingConnection, const FTimespan& WaitTime)
{
	bHasPendingConnection = false;
	return true;
}

bool FWeaponRelaySocket::HasPendingData(uint32& PendingDataSize)
{
	return HasPendingRelayData(PendingDataSize);
}

class FSocket* FWeaponRelaySocket::Accept(const FString& InSocketDescription)
{
	return nullptr;
}

class FSocket* FWeaponRelaySocket::Accept(FInternetAddr& OutAddr, const FString& InSocketDescription)
{
	return nullptr;
}

bool FWeaponRelaySocket::SendTo(const uint8* Data, int32 Count, int32& BytesSent, const FInternetAddr& Destination)
{
	if (!Underlying)
	{
		return false;
	}

	if (State == EWeaponRelaySocketState::Ready && bChannelBound && BoundPeerAddr.IsValid())
	{
		// ChannelData: 2-byte channel + 2-byte length + payload
		TArray<uint8> Out;
		WriteU16(Out, WeaponRelayStun::ChannelBase);
		WriteU16(Out, static_cast<uint16>(Count));
		Out.Append(Data, Count);

		int32 Sent = 0;
		const bool bOk = Underlying->SendTo(Out.GetData(), Out.Num(), Sent, *TurnServerAddr);
		BytesSent = bOk ? Count : 0;
		return bOk;
	}

	if (State == EWeaponRelaySocketState::Allocated && !bChannelBound)
	{
		// Kick off ChannelBind on first send to an unknown peer; the caller
		// will retry once Ready. Until the bind completes the data plane is
		// not available, so report the packet as sent without delivering it.
		BindChannel(Destination);
	}
	// Not Ready yet: absorb the packet. NetDriver's LowLevelSend treats a
	// false return as a hard send failure (which can cascade into repeated
	// reconnects); during the brief Allocating/Binding window dropping is
	// the correct behavior — the peer will retry via replication acks.
	BytesSent = Count;
	return true;
}

bool FWeaponRelaySocket::RecvFrom(uint8* Data, int32 BufferSize, int32& BytesRead, FInternetAddr& Source, ESocketReceiveFlags::Type Flags)
{
	// Drain buffered payload (e.g. from a Send Indication) first.
	if (ReadBufferOffset < ReadBuffer.Num())
	{
		const int32 Remaining = ReadBuffer.Num() - ReadBufferOffset;
		const int32 ToCopy = FMath::Min(Remaining, BufferSize);
		FMemory::Memcpy(Data, ReadBuffer.GetData() + ReadBufferOffset, ToCopy);
		ReadBufferOffset += ToCopy;
		BytesRead = ToCopy;
		if (BoundPeerAddr.IsValid())
		{
			Source = *BoundPeerAddr;
		}
		return true;
	}
	if (!Underlying)
	{
		return false;
	}

	TArray<uint8> Packet;
	Packet.SetNumUninitialized(4096);
	int32 PacketSize = 0;
	TSharedPtr<FInternetAddr> FromAddr = ISocketSubsystem::Get()->CreateInternetAddr();

	const bool bRead = Underlying->RecvFrom(Packet.GetData(), Packet.Num(), PacketSize, *FromAddr);
	if (!bRead)
	{
		return false;
	}
	if (PacketSize < 4)
	{
		BytesRead = 0;
		return true;
	}

	// ChannelData?
	const uint16 First = ReadU16(Packet.GetData());
	if ((First & WeaponRelayStun::ChannelDataMask) == WeaponRelayStun::ChannelDataMarker)
	{
		const uint16 PayloadLen = ReadU16(Packet.GetData() + 2);
		if (PayloadLen + 4 <= PacketSize && PayloadLen <= BufferSize)
		{
			FMemory::Memcpy(Data, Packet.GetData() + 4, PayloadLen);
			BytesRead = PayloadLen;
			if (BoundPeerAddr.IsValid())
			{
				Source = *BoundPeerAddr;
			}
			else
			{
				Source = *FromAddr;
			}
			return true;
		}
		BytesRead = 0;
		return true;
	}

	// STUN control response?
	if (PacketSize >= WeaponRelayStun::HeaderSize)
	{
		const uint16 MsgType = ReadU16(Packet.GetData());
		const uint16 MsgLen = ReadU16(Packet.GetData() + 2);
		const int32 AttrStart = 4 + 4 + 12;
		if (MsgLen + AttrStart <= PacketSize)
		{
			switch (MsgType)
			{
			case WeaponRelayStun::AllocateSuccess:
			case WeaponRelayStun::AllocateError:
				OnAllocateResponse(Packet.GetData(), MsgLen + AttrStart);
				break;
			case WeaponRelayStun::ChannelBindSuccess:
			case WeaponRelayStun::ChannelBindError:
				OnChannelBindResponse(Packet.GetData(), MsgLen + AttrStart);
				break;
			case WeaponRelayStun::DataIndication:
				OnDataIndication(Packet.GetData(), MsgLen + AttrStart);
				break;
			default:
				break;
			}
		}
	}

	BytesRead = 0;
	return true;
}

void FWeaponRelaySocket::OnDataIndication(const uint8* Data, int32 Size)
{
	// Peer sent us a Send Indication (data before ChannelBind). Extract the
	// payload and remember the peer's address for the bind.
	const int32 AttrStart = 4 + 4 + 12;
	int32 Offset = AttrStart;
	TArray<uint8> Payload;
	TSharedPtr<FInternetAddr> PeerAddr = ISocketSubsystem::Get()->CreateInternetAddr();
	while (Offset + 4 <= Size)
	{
		const uint16 AttrType = ReadU16(Data + Offset);
		const uint16 AttrLen = ReadU16(Data + Offset + 2);
		if (Offset + 4 + AttrLen > Size)
		{
			break;
		}
		const uint8* AttrData = Data + Offset + 4;
		if (AttrType == WeaponRelayStun::AttrXorPeerAddress && AttrLen >= 8)
		{
			DecodeXorAddr(AttrData, Data + 8, *PeerAddr);
		}
		else if (AttrType == WeaponRelayStun::AttrData)
		{
			Payload.SetNumUninitialized(AttrLen);
			FMemory::Memcpy(Payload.GetData(), AttrData, AttrLen);
		}
		Offset += 4 + ((AttrLen + 3) & ~3);
	}

	if (Payload.Num() > 0)
	{
		// Buffer the inbound payload for the next RecvFrom.
		ReadBuffer.Reset();
		ReadBuffer.Append(Payload.GetData(), Payload.Num());
		ReadBufferOffset = 0;

		// Kick off ChannelBind toward this peer on first contact.
		if (State == EWeaponRelaySocketState::Allocated && !bChannelBound)
		{
			BindChannel(*PeerAddr);
		}
	}
}

bool FWeaponRelaySocket::Wait(ESocketWaitConditions::Type Condition, FTimespan WaitTime)
{
	return Underlying ? Underlying->Wait(Condition, WaitTime) : false;
}

ESocketConnectionState FWeaponRelaySocket::GetConnectionState()
{
	return (State == EWeaponRelaySocketState::Ready) ? SCS_Connected : SCS_NotConnected;
}

void FWeaponRelaySocket::GetAddress(FInternetAddr& OutAddr)
{
	if (RelayedAddress.IsValid())
	{
		OutAddr = *RelayedAddress;
	}
	else if (Underlying)
	{
		Underlying->GetAddress(OutAddr);
	}
}

bool FWeaponRelaySocket::GetPeerAddress(FInternetAddr& OutAddr)
{
	if (BoundPeerAddr.IsValid())
	{
		OutAddr = *BoundPeerAddr;
		return true;
	}
	return false;
}

bool FWeaponRelaySocket::SetNonBlocking(bool bIsNonBlocking)
{
	bNonBlocking = bIsNonBlocking;
	return Underlying ? Underlying->SetNonBlocking(bIsNonBlocking) : false;
}

bool FWeaponRelaySocket::SetBroadcast(bool bAllowBroadcast)
{
	return Underlying ? Underlying->SetBroadcast(bAllowBroadcast) : false;
}

bool FWeaponRelaySocket::SetNoDelay(bool bIsNoDelay)
{
	return Underlying ? Underlying->SetNoDelay(bIsNoDelay) : false;
}

bool FWeaponRelaySocket::JoinMulticastGroup(const FInternetAddr& GroupAddress)
{
	return false;
}

bool FWeaponRelaySocket::JoinMulticastGroup(const FInternetAddr& GroupAddress, const FInternetAddr& InterfaceAddress)
{
	return false;
}

bool FWeaponRelaySocket::LeaveMulticastGroup(const FInternetAddr& GroupAddress)
{
	return false;
}

bool FWeaponRelaySocket::LeaveMulticastGroup(const FInternetAddr& GroupAddress, const FInternetAddr& InterfaceAddress)
{
	return false;
}

bool FWeaponRelaySocket::SetMulticastLoopback(bool bLoopback)
{
	return false;
}

bool FWeaponRelaySocket::SetMulticastTtl(uint8 TimeToLive)
{
	return false;
}

bool FWeaponRelaySocket::SetMulticastInterface(const FInternetAddr& InterfaceAddress)
{
	return false;
}

bool FWeaponRelaySocket::SetReuseAddr(bool bAllowReuse)
{
	return Underlying ? Underlying->SetReuseAddr(bAllowReuse) : false;
}

bool FWeaponRelaySocket::SetLinger(bool bShouldLinger, int32 Timeout)
{
	return Underlying ? Underlying->SetLinger(bShouldLinger, Timeout) : false;
}

bool FWeaponRelaySocket::SetRecvErr(bool bUseErrorQueue)
{
	return Underlying ? Underlying->SetRecvErr(bUseErrorQueue) : false;
}

bool FWeaponRelaySocket::SetSendBufferSize(int32 Size, int32& OutNewSize)
{
	return Underlying ? Underlying->SetSendBufferSize(Size, OutNewSize) : false;
}

bool FWeaponRelaySocket::SetReceiveBufferSize(int32 Size, int32& OutNewSize)
{
	return Underlying ? Underlying->SetReceiveBufferSize(Size, OutNewSize) : false;
}

int32 FWeaponRelaySocket::GetPortNo()
{
	if (Underlying)
	{
		return Underlying->GetPortNo();
	}
	return 0;
}

// ---- 内部:STUN 事务 ----

bool FWeaponRelaySocket::SendStunTransaction(const TArray<uint8>& Message)
{
	if (!Underlying || !TurnServerAddr.IsValid())
	{
		return false;
	}
	int32 Sent = 0;
	return Underlying->SendTo(Message.GetData(), Message.Num(), Sent, *TurnServerAddr);
}

void FWeaponRelaySocket::OnAllocateResponse(const uint8* Data, int32 Size)
{
	bTransactionPending = false;
	const uint16 MsgType = ReadU16(Data);

	// 401: server wants authentication. Capture NONCE/REALM and retry.
	if (MsgType == WeaponRelayStun::AllocateError)
	{
		const int32 AttrStart = 4 + 4 + 12;
		int32 Offset = AttrStart;
		bool bUnauthorized = false;
		while (Offset + 4 <= Size)
		{
			const uint16 AttrType = ReadU16(Data + Offset);
			const uint16 AttrLen = ReadU16(Data + Offset + 2);
			if (Offset + 4 + AttrLen > Size)
			{
				break;
			}
			const uint8* AttrData = Data + Offset + 4;
			if (AttrType == WeaponRelayStun::AttrErrorCode && AttrLen >= 4)
			{
				const uint16 Code = static_cast<uint16>(AttrData[2] * 100 + AttrData[3]);
				bUnauthorized = (Code == 401);
			}
			else if (AttrType == WeaponRelayStun::AttrNonce)
			{
				Nonce = FString(reinterpret_cast<const TCHAR*>(AttrData), AttrLen); // ASCII nonce
			}
			else if (AttrType == WeaponRelayStun::AttrRealm)
			{
				Realm = FString(reinterpret_cast<const TCHAR*>(AttrData), AttrLen);
			}
			Offset += 4 + ((AttrLen + 3) & ~3);
		}

		if (bUnauthorized && !Nonce.IsEmpty())
		{
			UE_LOG(LogWeaponRelay, Log, TEXT("TURN Allocate challenged (nonce from %s)"), *Realm);
			SendAuthenticatedAllocate();
			return;
		}

		UE_LOG(LogWeaponRelay, Error, TEXT("Allocate rejected by TURN server"));
		State = EWeaponRelaySocketState::Failed;
		return;
	}

	if (MsgType != WeaponRelayStun::AllocateSuccess)
	{
		State = EWeaponRelaySocketState::Failed;
		return;
	}

	// parse attributes for XOR-RELAYED-ADDRESS
	const int32 AttrStart = 4 + 4 + 12;
	int32 Offset = AttrStart;
	while (Offset + 4 <= Size)
	{
		const uint16 AttrType = ReadU16(Data + Offset);
		const uint16 AttrLen = ReadU16(Data + Offset + 2);
		if (Offset + 4 + AttrLen > Size)
		{
			break;
		}
		const uint8* AttrData = Data + Offset + 4;
		if (AttrType == WeaponRelayStun::AttrXorRelayedAddress && AttrLen >= 8)
		{
			RelayedAddress = ISocketSubsystem::Get()->CreateInternetAddr();
			DecodeXorAddr(AttrData, Data + 8, *RelayedAddress);
			State = EWeaponRelaySocketState::Allocated;
			bAllocateAuthenticated = true;
			UE_LOG(LogWeaponRelay, Log, TEXT("TURN allocated relayed address %s"), *RelayedAddress->ToString(true));
			return;
		}
		Offset += 4 + ((AttrLen + 3) & ~3);
	}

	UE_LOG(LogWeaponRelay, Error, TEXT("Allocate success without XOR-RELAYED-ADDRESS"));
	State = EWeaponRelaySocketState::Failed;
}

void FWeaponRelaySocket::SendAuthenticatedAllocate()
{
	TArray<uint8> Msg;
	WriteU16(Msg, WeaponRelayStun::AllocateRequest);
	WriteU16(Msg, 0);
	WriteU32(Msg, WeaponRelayStun::MagicCookie32);
	for (int32 i = 0; i < 12; ++i)
	{
		Msg.Add(FMath::Rand() & 0xFF);
	}
	// REQUESTED-TRANSPORT: UDP
	WriteU16(Msg, WeaponRelayStun::AttrRequestedTransport);
	WriteU16(Msg, 4);
	Msg.Add(17); Msg.Add(0); Msg.Add(0); Msg.Add(0);
	// USERNAME
	FTCHARToUTF8 UsernameUtf8(*Username);
	WriteU16(Msg, WeaponRelayStun::AttrUsername);
	WriteU16(Msg, static_cast<uint16>(UsernameUtf8.Length()));
	for (int32 i = 0; i < UsernameUtf8.Length(); ++i)
	{
		Msg.Add(static_cast<uint8>(UsernameUtf8.Get()[i]));
	}
	// REALM
	FTCHARToUTF8 RealmUtf8(*Realm);
	WriteU16(Msg, WeaponRelayStun::AttrRealm);
	WriteU16(Msg, static_cast<uint16>(RealmUtf8.Length()));
	for (int32 i = 0; i < RealmUtf8.Length(); ++i)
	{
		Msg.Add(static_cast<uint8>(RealmUtf8.Get()[i]));
	}
	// NONCE
	FTCHARToUTF8 NonceUtf8(*Nonce);
	WriteU16(Msg, WeaponRelayStun::AttrNonce);
	WriteU16(Msg, static_cast<uint16>(NonceUtf8.Length()));
	for (int32 i = 0; i < NonceUtf8.Length(); ++i)
	{
		Msg.Add(static_cast<uint8>(NonceUtf8.Get()[i]));
	}
	// MESSAGE-INTEGRITY
	AppendMessageIntegrity(Msg, MakeAuthKey(Username, Realm, Password));
	const uint16 Len = static_cast<uint16>(Msg.Num() - WeaponRelayStun::HeaderSize);
	Msg[2] = static_cast<uint8>(Len >> 8);
	Msg[3] = static_cast<uint8>(Len & 0xFF);

	TransactionStartTime = FPlatformTime::Seconds();
	bTransactionPending = true;
	SendStunTransaction(Msg);
}

void FWeaponRelaySocket::OnChannelBindResponse(const uint8* Data, int32 Size)
{
	bTransactionPending = false;
	const uint16 MsgType = ReadU16(Data);
	if (MsgType == WeaponRelayStun::ChannelBindSuccess)
	{
		bChannelBound = true;
		State = EWeaponRelaySocketState::Ready;
		UE_LOG(LogWeaponRelay, Log, TEXT("Channel bound to %s"), BoundPeerAddr.IsValid() ? *BoundPeerAddr->ToString(true) : TEXT("?"));
	}
	else
	{
		UE_LOG(LogWeaponRelay, Error, TEXT("ChannelBind rejected"));
		State = EWeaponRelaySocketState::Failed;
	}
}

// ---- 内部:ChannelData ----

