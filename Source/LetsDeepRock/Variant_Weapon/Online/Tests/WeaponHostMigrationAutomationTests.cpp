// Copyright Epic Games, Inc. All Rights Reserved.

#include "Misc/AutomationTest.h"

#if WITH_DEV_AUTOMATION_TESTS

#include "Attributes/AttributeModifierManager.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GridMap/GridMapGenerator.h"
#include "Net/OnlineEngineInterface.h"
#include "Online/WeaponHostMigrationSubsystem.h"
#include "Online/WeaponMatchSaveGame.h"
#include "Online/WeaponSnapshotCoordinator.h"

namespace WeaponHostMigrationTests
{
constexpr int32 TestGridWidth = 64;
constexpr int32 TestGridHeight = 64;
constexpr int32 TestGridDeltaCount = 2048;

UGameInstance* FindGameInstance()
{
	if (!GEngine)
	{
		return nullptr;
	}

	for (const FWorldContext& WorldContext : GEngine->GetWorldContexts())
	{
		UWorld* World = WorldContext.World();
		if (World && World->GetGameInstance())
		{
			return World->GetGameInstance();
		}
	}
	return nullptr;
}

FUniqueNetIdRepl MakePlayerId(const TCHAR* Contents)
{
	UOnlineEngineInterface* OnlineEngine = UOnlineEngineInterface::Get();
	return OnlineEngine
		? FUniqueNetIdRepl(OnlineEngine->CreateUniquePlayerIdWrapper(Contents))
		: FUniqueNetIdRepl();
}

FWeaponMigrationRoleMember MakeRoleMember(const FUniqueNetIdRepl& PlayerId, const int32 Slot)
{
	FWeaponMigrationRoleMember Member;
	Member.PlayerId = PlayerId;
	Member.OriginalSlot = Slot;
	return Member;
}

UWeaponMatchSaveGame* MakeSaveGame(
	const FGuid& MatchId,
	const int64 SnapshotRevision,
	const uint64 RosterRevision,
	const FUniqueNetIdRepl& PlayerId,
	FString& OutError)
{
	OutError.Reset();
	const TMap<FGameplayTag, FGameplayAttribute>& AttributeMap =
		UAttributeModifierManager::GetAttributeTagMap();
	if (!PlayerId.IsValid() || AttributeMap.IsEmpty())
	{
		OutError = TEXT("Automation fixture requires a valid player identity and attribute map");
		return nullptr;
	}

	UWeaponMatchSaveGame* SaveGame = NewObject<UWeaponMatchSaveGame>(GetTransientPackage());
	SaveGame->MapPackageName = TEXT("/Game/Variant_Weapon/LVL_Weapon/NewMap");
	SaveGame->MaxPlayers = 4;

	FWeaponSnapshot& Snapshot = SaveGame->Snapshot;
	Snapshot.MatchId = MatchId;
	Snapshot.Revision = SnapshotRevision;
	Snapshot.MatchPhase = EWeaponMatchPhase::Waiting;
	Snapshot.RemainingPhaseTime = 0.0f;
	Snapshot.RosterRevision = RosterRevision;

	FWeaponRecoveryRosterSnapshot& RosterEntry = Snapshot.RecoveryRoster.AddDefaulted_GetRef();
	RosterEntry.PlayerId = PlayerId;
	RosterEntry.OriginalSlot = 0;
	RosterEntry.Status = EWeaponRecoveryPlayerStatus::Ready;

	const FGameplayTag AttributeTag = AttributeMap.CreateConstIterator()->Key;
	FWeaponPlayerSnapshot& Player = Snapshot.Players.AddDefaulted_GetRef();
	Player.PlayerId = PlayerId;
	Player.PlayerName = TEXT("MigrationAutomationPlayer");
	Player.SlotIndex = 0;
	Player.bIsReady = true;
	Player.bHasPawn = true;
	Player.TeamId = 1;
	Player.Location = FVector(100.0, 200.0, 50.0);
	Player.Rotation = FRotator(0.0, 30.0, 0.0);
	Player.Velocity = FVector(10.0, 20.0, 0.0);
	Player.CharacterConfigRowName = TEXT("Default");
	Player.CurrentWeaponIndex = 0;
	Player.AttributeState.Values.Add(AttributeTag, 100.0f);

	FWeaponEnemySnapshot& Enemy = Snapshot.Enemies.AddDefaulted_GetRef();
	Enemy.EnemyClass = FSoftClassPath(
		TEXT("/Game/Variant_Weapon/Enemies/BP_Enemy_BasicGrunt.BP_Enemy_BasicGrunt_C"));
	Enemy.Location = FVector(400.0, 500.0, 50.0);
	Enemy.Rotation = FRotator(0.0, 90.0, 0.0);
	Enemy.Velocity = FVector::ZeroVector;
	Enemy.EnemyConfigRowName = TEXT("BasicGrunt");
	Enemy.AttributeState.Values.Add(AttributeTag, 75.0f);

	FGridMapRecoveryState& GridMap = Snapshot.GridMap;
	GridMap.GenerationVersion = 1;
	GridMap.Config.MapWidth = TestGridWidth;
	GridMap.Config.MapHeight = TestGridHeight;
	GridMap.Config.TileSize = 100.0f;
	GridMap.Config.TreeHealth = 100;
	GridMap.Config.LargeTreeHealth = 300;
	GridMap.GeneratorClass = FSoftClassPath(UGridMapGenerator_Simple::StaticClass());
	GridMap.EffectiveSeed = 12345;
	GridMap.bGenerated = true;
	GridMap.TileDeltas.Reserve(TestGridDeltaCount);
	for (int32 Index = 0; Index < TestGridDeltaCount; ++Index)
	{
		FGridTileRecoveryState& Delta = GridMap.TileDeltas.AddDefaulted_GetRef();
		Delta.GridX = Index % TestGridWidth;
		Delta.GridY = Index / TestGridWidth;
		Delta.TileType = EGridTileType::Empty;
		Delta.GenerationVersion = GridMap.GenerationVersion;
		Delta.Revision = static_cast<uint32>(Index + 1);
	}
	GridMap.NextTileRevision = TestGridDeltaCount;

	FWeaponProjectileSnapshot& Projectile = Snapshot.Projectiles.AddDefaulted_GetRef();
	Projectile.ProjectileClass = FSoftClassPath(
		TEXT("/Game/Variant_Weapon/BluePrints/BP_WeaponProjectile.BP_WeaponProjectile_C"));
	Projectile.DamageEffectClass = FSoftClassPath(
		TEXT("/Game/Variant_Weapon/BluePrints/GE_Damage.GE_Damage_C"));
	Projectile.InstigatorPlayerId = PlayerId;
	Projectile.Location = FVector(600.0, 700.0, 100.0);
	Projectile.Rotation = FRotator(0.0, 45.0, 0.0);
	Projectile.Velocity = FVector(700.0, 700.0, 0.0);
	Projectile.DamageValue = 10.0f;
	Projectile.ProjectileSpeed = 1000.0f;
	Projectile.RemainingLifeSeconds = 120.0f;

	FWeaponGrenadeSnapshot& Grenade = Snapshot.Grenades.AddDefaulted_GetRef();
	Grenade.GrenadeDataAsset = FSoftObjectPath(
		TEXT("/Game/Variant_Weapon/Weapons/DA_Grenade_Default.DA_Grenade_Default"));
	Grenade.InstigatorPlayerId = PlayerId;
	Grenade.Location = FVector(800.0, 900.0, 100.0);
	Grenade.Rotation = FRotator(15.0, 60.0, 0.0);
	Grenade.Velocity = FVector(500.0, 800.0, 300.0);
	Grenade.RemainingFuseTime = 120.0f;

	if (!UWeaponSnapshotCoordinator::ValidateSaveGame(SaveGame, OutError))
	{
		return nullptr;
	}
	return SaveGame;
}

TArray<uint8> CopyChunk(
	const TArray<uint8>& Payload,
	const int32 ChunkIndex,
	const int32 ChunkPayloadBytes)
{
	const int32 Offset = ChunkIndex * ChunkPayloadBytes;
	const int32 ByteCount = FMath::Min(ChunkPayloadBytes, Payload.Num() - Offset);
	TArray<uint8> Chunk;
	Chunk.Append(Payload.GetData() + Offset, ByteCount);
	return Chunk;
}

bool StoreChunk(
	UWeaponHostMigrationSubsystem* Subsystem,
	const FWeaponMigrationPayloadEnvelope& BaseEnvelope,
	const TArray<uint8>& Payload,
	const int32 ChunkIndex,
	bool& bOutComplete,
	FString& OutError)
{
	FWeaponMigrationPayloadEnvelope ChunkEnvelope = BaseEnvelope;
	ChunkEnvelope.ChunkIndex = ChunkIndex;
	return Subsystem->StorePendingChunk(
		ChunkEnvelope,
		CopyChunk(Payload, ChunkIndex, Subsystem->GetChunkPayloadBytes()),
		bOutComplete,
		OutError);
}

bool StoreAllChunks(
	UWeaponHostMigrationSubsystem* Subsystem,
	const FWeaponMigrationPayloadEnvelope& Envelope,
	const TArray<uint8>& Payload,
	FString& OutError)
{
	bool bComplete = false;
	for (int32 ChunkIndex = Envelope.ChunkCount - 1; ChunkIndex >= 0; --ChunkIndex)
	{
		if (!StoreChunk(Subsystem, Envelope, Payload, ChunkIndex, bComplete, OutError))
		{
			return false;
		}
	}
	return bComplete;
}
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWeaponMigrationPayloadBufferAutomationTest,
	"LetsDeepRock.Weapon.HostMigration.PayloadBuffer",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWeaponMigrationPayloadBufferAutomationTest::RunTest(const FString& Parameters)
{
	using namespace WeaponHostMigrationTests;

	UGameInstance* GameInstance = FindGameInstance();
	if (!GameInstance)
	{
		AddError(TEXT("A running GameInstance is required; run this test with NewMap loaded"));
		return false;
	}
	UWeaponSnapshotCoordinator* Coordinator =
		GameInstance->GetSubsystem<UWeaponSnapshotCoordinator>();
	if (!Coordinator)
	{
		AddError(TEXT("WeaponSnapshotCoordinator subsystem is unavailable"));
		return false;
	}

	const FUniqueNetIdRepl PlayerId = MakePlayerId(TEXT("1111111111111111"));
	const FGuid MatchId = FGuid::NewGuid();
	FString Error;
	UWeaponMatchSaveGame* SaveGame = MakeSaveGame(MatchId, 7, 11, PlayerId, Error);
	if (!TestNotNull(*Error, SaveGame))
	{
		return false;
	}

	TArray<uint8> Payload;
	if (!TestTrue(*Error, UWeaponSnapshotCoordinator::SerializeSaveGame(SaveGame, Payload, Error)))
	{
		return false;
	}

	UWeaponHostMigrationSubsystem* Subsystem =
		NewObject<UWeaponHostMigrationSubsystem>(GameInstance);
	FWeaponMigrationPayloadEnvelope Envelope;
	if (!TestTrue(
		TEXT("A valid SaveGame builds a migration envelope"),
		Subsystem->BuildPayloadEnvelope(SaveGame, Payload, 5, Envelope, Error)))
	{
		AddError(Error);
		return false;
	}
	const TArray<uint8> KnownSHA256Input = {'a', 'b', 'c'};
	const TArray<uint8> KnownSHA256Digest = {
		0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea,
		0x41, 0x41, 0x40, 0xde, 0x5d, 0xae, 0x22, 0x23,
		0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17, 0x7a, 0x9c,
		0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
	FWeaponMigrationPayloadEnvelope KnownSHA256Envelope;
	if (!TestTrue(
		TEXT("The migration hash implementation accepts the SHA-256 known vector"),
		Subsystem->BuildPayloadEnvelope(
			SaveGame, KnownSHA256Input, 5, KnownSHA256Envelope, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(
		TEXT("SHA-256('abc') matches the standard 32-byte digest"),
		KnownSHA256Envelope.PayloadHash == KnownSHA256Digest);
	if (!TestTrue(TEXT("The fixture spans multiple chunks"), Envelope.ChunkCount > 1))
	{
		return false;
	}

	bool bComplete = false;
	for (int32 ChunkIndex = Envelope.ChunkCount - 1; ChunkIndex > 0; --ChunkIndex)
	{
		if (!TestTrue(
			TEXT("Out-of-order incomplete chunk is accepted into Pending"),
			StoreChunk(Subsystem, Envelope, Payload, ChunkIndex, bComplete, Error)))
		{
			AddError(Error);
			return false;
		}
	}
	FWeaponMigrationPayloadAck Ack;
	TestFalse(TEXT("Incomplete Pending cannot promote"), Subsystem->PromotePendingPayload(Ack, Error));
	TestFalse(TEXT("Incomplete Pending cannot replace Committed"), Subsystem->HasCommittedPayload());

	if (!TestTrue(
		TEXT("The final missing chunk completes Pending"),
		StoreChunk(Subsystem, Envelope, Payload, 0, bComplete, Error)) ||
		!TestTrue(TEXT("Pending reports complete"), bComplete) ||
		!TestTrue(TEXT("Complete hash-valid Pending promotes"), Subsystem->PromotePendingPayload(Ack, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Promotion ACK identifies the payload"), Ack.IdentifiesPayload(Envelope));
	TArray<uint8> CommittedBytes;
	TestTrue(TEXT("Committed bytes are readable"), Subsystem->CopyCommittedPayload(CommittedBytes));
	TestTrue(TEXT("Committed bytes are exact"), CommittedBytes == Payload);

	UWeaponMatchSaveGame* RoundTrippedSaveGame = nullptr;
	if (!TestTrue(
		TEXT("Promoted bytes deserialize through the production coordinator"),
		Coordinator->DeserializeSaveGame(CommittedBytes, RoundTrippedSaveGame, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("GridMap deltas survive the byte round trip"),
		RoundTrippedSaveGame->Snapshot.GridMap.TileDeltas.Num(), TestGridDeltaCount);
	TestEqual(TEXT("Enemy state survives the byte round trip"),
		RoundTrippedSaveGame->Snapshot.Enemies.Num(), 1);
	TestEqual(TEXT("Projectile state survives the byte round trip"),
		RoundTrippedSaveGame->Snapshot.Projectiles.Num(), 1);
	TestEqual(TEXT("Grenade state survives the byte round trip"),
		RoundTrippedSaveGame->Snapshot.Grenades.Num(), 1);

	UWeaponMatchSaveGame* NewerSaveGame = DuplicateObject<UWeaponMatchSaveGame>(
		SaveGame, GetTransientPackage());
	NewerSaveGame->Snapshot.Revision = 8;
	TArray<uint8> NewerPayload;
	if (!TestTrue(
		TEXT("The newer fixture serializes"),
		UWeaponSnapshotCoordinator::SerializeSaveGame(NewerSaveGame, NewerPayload, Error)))
	{
		AddError(Error);
		return false;
	}
	FWeaponMigrationPayloadEnvelope NewerEnvelope;
	if (!TestTrue(
		TEXT("The newer fixture builds an envelope"),
		Subsystem->BuildPayloadEnvelope(NewerSaveGame, NewerPayload, 5, NewerEnvelope, Error)))
	{
		AddError(Error);
		return false;
	}
	UWeaponMatchSaveGame* NewestSaveGame = DuplicateObject<UWeaponMatchSaveGame>(
		NewerSaveGame, GetTransientPackage());
	NewestSaveGame->Snapshot.Revision = 9;
	TArray<uint8> NewestPayload;
	FWeaponMigrationPayloadEnvelope NewestEnvelope;
	if (!TestTrue(
		TEXT("The newest fixture serializes"),
		UWeaponSnapshotCoordinator::SerializeSaveGame(NewestSaveGame, NewestPayload, Error)) ||
		!TestTrue(
			TEXT("The newest fixture builds an envelope"),
			Subsystem->BuildPayloadEnvelope(
				NewestSaveGame, NewestPayload, 5, NewestEnvelope, Error)))
	{
		AddError(Error);
		return false;
	}
	if (!TestTrue(
		TEXT("Revision 8 starts an incomplete Pending payload"),
		StoreChunk(Subsystem, NewerEnvelope, NewerPayload, 0, bComplete, Error)) ||
		!TestTrue(
			TEXT("Revision 9 replaces the incomplete older Pending payload"),
			StoreChunk(Subsystem, NewestEnvelope, NewestPayload, 0, bComplete, Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(
		TEXT("A chunk from replaced revision 8 cannot re-enter revision 9 Pending"),
		StoreChunk(Subsystem, NewerEnvelope, NewerPayload, 1, bComplete, Error));
	TestTrue(TEXT("Rejecting the replaced revision preserves the newer Pending payload"),
		Subsystem->HasPendingPayload());
	Subsystem->DiscardPendingPayload();

	TArray<uint8> CorruptPayload = NewerPayload;
	CorruptPayload.Last() ^= 0x5a;
	if (!TestTrue(
		TEXT("A complete corrupt payload reaches integrity validation"),
		StoreAllChunks(Subsystem, NewerEnvelope, CorruptPayload, Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("A corrupt payload cannot promote"), Subsystem->PromotePendingPayload(Ack, Error));
	TestTrue(TEXT("Corruption preserves the prior Committed bytes"),
		Subsystem->CopyCommittedPayload(CommittedBytes));
	TestTrue(TEXT("Prior Committed remains byte-identical after corruption"), CommittedBytes == Payload);

	Subsystem->DiscardPendingPayload();
	if (!TestTrue(
		TEXT("The first copy of a duplicate chunk is accepted"),
		StoreChunk(Subsystem, NewerEnvelope, NewerPayload, 0, bComplete, Error)))
	{
		AddError(Error);
		return false;
	}
	FWeaponMigrationPayloadEnvelope FirstChunkEnvelope = NewerEnvelope;
	FirstChunkEnvelope.ChunkIndex = 0;
	if (!TestTrue(
		TEXT("An identical duplicate chunk is idempotently accepted"),
		Subsystem->StorePendingChunk(
			FirstChunkEnvelope,
			CopyChunk(NewerPayload, 0, Subsystem->GetChunkPayloadBytes()),
			bComplete,
			Error)))
	{
		AddError(Error);
		return false;
	}
	TArray<uint8> ConflictingDuplicate = CopyChunk(
		NewerPayload, 0, Subsystem->GetChunkPayloadBytes());
	ConflictingDuplicate[0] ^= 0x01;
	TestFalse(
		TEXT("A conflicting duplicate invalidates Pending"),
		Subsystem->StorePendingChunk(
			FirstChunkEnvelope, ConflictingDuplicate, bComplete, Error));
	TestFalse(TEXT("Conflicting duplicate discards Pending"), Subsystem->HasPendingPayload());
	TestTrue(TEXT("Conflicting duplicate preserves Committed"), Subsystem->HasCommittedPayload());

	if (!TestTrue(
		TEXT("A valid newer payload assembles"),
		StoreAllChunks(Subsystem, NewerEnvelope, NewerPayload, Error)) ||
		!TestTrue(
			TEXT("A valid newer payload promotes"),
			Subsystem->PromotePendingPayload(Ack, Error)))
	{
		AddError(Error);
		return false;
	}
	TestEqual(TEXT("The newer revision becomes locally Committed"), Ack.SnapshotRevision, int64{8});

	TestFalse(
		TEXT("A payload older than Committed is rejected"),
		StoreChunk(Subsystem, Envelope, Payload, 0, bComplete, Error));

	FWeaponMigrationPayloadEnvelope OversizedEnvelope = NewerEnvelope;
	OversizedEnvelope.PayloadByteCount = Subsystem->GetMaxSnapshotBytes() + 1;
	OversizedEnvelope.ChunkCount = FMath::DivideAndRoundUp(
		OversizedEnvelope.PayloadByteCount, Subsystem->GetChunkPayloadBytes());
	TestFalse(
		TEXT("An oversized envelope fails closed"),
		StoreChunk(Subsystem, OversizedEnvelope, NewerPayload, 0, bComplete, Error));

	FWeaponMigrationPayloadEnvelope IncompatibleEnvelope = NewerEnvelope;
	++IncompatibleEnvelope.ProtocolVersion;
	TestFalse(
		TEXT("An incompatible protocol version fails closed"),
		StoreChunk(Subsystem, IncompatibleEnvelope, NewerPayload, 0, bComplete, Error));

	FWeaponMigrationPayloadEnvelope MismatchedIdentityEnvelope = NewerEnvelope;
	++MismatchedIdentityEnvelope.SnapshotRevision;
	if (!TestTrue(
		TEXT("An envelope/data identity mismatch reaches SaveGame validation"),
		StoreAllChunks(Subsystem, MismatchedIdentityEnvelope, NewerPayload, Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(
		TEXT("An envelope/data identity mismatch cannot promote"),
		Subsystem->PromotePendingPayload(Ack, Error));
	TestEqual(TEXT("Failed identity validation preserves revision 8"),
		Subsystem->GetCommittedEnvelope()->SnapshotRevision, int64{8});

	TArray<uint8> NonSaveGameBytes;
	NonSaveGameBytes.SetNumUninitialized(256);
	for (int32 Index = 0; Index < NonSaveGameBytes.Num(); ++Index)
	{
		NonSaveGameBytes[Index] = static_cast<uint8>(Index);
	}
	UWeaponMatchSaveGame* OpaqueEnvelopeSaveGame = DuplicateObject<UWeaponMatchSaveGame>(
		NewerSaveGame, GetTransientPackage());
	OpaqueEnvelopeSaveGame->Snapshot.Revision = 9;
	FWeaponMigrationPayloadEnvelope NonSaveGameEnvelope;
	if (!TestTrue(
		TEXT("Opaque bytes can be integrity-wrapped before receiver validation"),
		Subsystem->BuildPayloadEnvelope(
			OpaqueEnvelopeSaveGame, NonSaveGameBytes, 5, NonSaveGameEnvelope, Error)) ||
		!TestTrue(
			TEXT("Opaque bytes assemble"),
			StoreAllChunks(Subsystem, NonSaveGameEnvelope, NonSaveGameBytes, Error)))
	{
		AddError(Error);
		return false;
	}
	TestFalse(TEXT("Non-SaveGame bytes fail closed at promotion"),
		Subsystem->PromotePendingPayload(Ack, Error));
	TestEqual(TEXT("Failed deserialization preserves revision 8"),
		Subsystem->GetCommittedEnvelope()->SnapshotRevision, int64{8});

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWeaponMigrationRoleFencingAutomationTest,
	"LetsDeepRock.Weapon.HostMigration.RoleFencing",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWeaponMigrationRoleFencingAutomationTest::RunTest(const FString& Parameters)
{
	using namespace WeaponHostMigrationTests;

	UGameInstance* GameInstance = FindGameInstance();
	if (!GameInstance)
	{
		AddError(TEXT("A running GameInstance is required; run this test with NewMap loaded"));
		return false;
	}

	const FUniqueNetIdRepl HostId = MakePlayerId(TEXT("1111111111111111"));
	const FUniqueNetIdRepl BackupAId = MakePlayerId(TEXT("2222222222222222"));
	const FUniqueNetIdRepl BackupBId = MakePlayerId(TEXT("3333333333333333"));
	if (!TestTrue(TEXT("Synthetic Null identities are valid"),
		HostId.IsValid() && BackupAId.IsValid() && BackupBId.IsValid()))
	{
		return false;
	}

	const FGuid MatchId = FGuid::NewGuid();
	const FWeaponMigrationRoleMember Host = MakeRoleMember(HostId, 0);
	const FWeaponMigrationRoleMember BackupA = MakeRoleMember(BackupAId, 1);
	const FWeaponMigrationRoleMember BackupB = MakeRoleMember(BackupBId, 2);

	FWeaponMigrationRoleAssignment Assignment;
	Assignment.MatchId = MatchId;
	Assignment.AuthorityEpoch = 1;
	Assignment.RosterRevision = 17;
	Assignment.ActiveHost = Host;
	Assignment.BackupA = BackupA;
	Assignment.BackupB = BackupB;

	FString Error;
	uint64 Epoch = 0;
	UWeaponHostMigrationSubsystem* Authority =
		NewObject<UWeaponHostMigrationSubsystem>(GameInstance);
	if (!TestTrue(
		TEXT("A new match prepares Epoch 1"),
		Authority->PrepareAuthorityTerm(MatchId, Host, 17, Epoch, Error)) ||
		!TestTrue(TEXT("New match Epoch is 1"), Epoch == 1) ||
		!TestTrue(
			TEXT("The authoritative complete assignment publishes"),
			Authority->SetAuthorityRoleAssignment(Assignment, Error)))
	{
		AddError(Error);
		return false;
	}
	TestTrue(TEXT("Host role resolves"),
		Authority->GetRoleForPlayer(HostId) == EWeaponMigrationRole::ActiveHost);
	TestTrue(TEXT("Backup A role resolves"),
		Authority->GetRoleForPlayer(BackupAId) == EWeaponMigrationRole::BackupA);
	TestTrue(TEXT("Backup B role resolves"),
		Authority->GetRoleForPlayer(BackupBId) == EWeaponMigrationRole::BackupB);

	UWeaponHostMigrationSubsystem* Receiver =
		NewObject<UWeaponHostMigrationSubsystem>(GameInstance);
	if (!TestTrue(TEXT("A receiver accepts the initial assignment"),
		Receiver->AcceptRoleAssignment(Assignment, Error)))
	{
		AddError(Error);
		return false;
	}

	FWeaponMigrationRoleAssignment StaleRoster = Assignment;
	--StaleRoster.RosterRevision;
	TestFalse(TEXT("RosterRevision cannot move backwards"),
		Receiver->AcceptRoleAssignment(StaleRoster, Error));

	FWeaponMigrationRoleAssignment SameRevisionConflict = Assignment;
	Swap(SameRevisionConflict.BackupA, SameRevisionConflict.BackupB);
	TestFalse(TEXT("One RosterRevision cannot contain conflicting roles"),
		Receiver->AcceptRoleAssignment(SameRevisionConflict, Error));

	FWeaponMigrationRoleAssignment SuccessorA;
	SuccessorA.MatchId = MatchId;
	SuccessorA.AuthorityEpoch = 2;
	SuccessorA.RosterRevision = 17;
	SuccessorA.ActiveHost = BackupA;
	TestTrue(TEXT("Backup A is the deterministic old+1 successor"),
		Receiver->AcceptRoleAssignment(SuccessorA, Error));
	TestFalse(TEXT("An older Epoch cannot overwrite the successor"),
		Receiver->AcceptRoleAssignment(Assignment, Error));

	UWeaponHostMigrationSubsystem* ReceiverB =
		NewObject<UWeaponHostMigrationSubsystem>(GameInstance);
	TestTrue(TEXT("A second receiver accepts the initial assignment"),
		ReceiverB->AcceptRoleAssignment(Assignment, Error));
	FWeaponMigrationRoleAssignment SuccessorB;
	SuccessorB.MatchId = MatchId;
	SuccessorB.AuthorityEpoch = 3;
	SuccessorB.RosterRevision = 17;
	SuccessorB.ActiveHost = BackupB;
	TestTrue(TEXT("Backup B is the deterministic old+2 successor"),
		ReceiverB->AcceptRoleAssignment(SuccessorB, Error));

	UWeaponHostMigrationSubsystem* InvalidSuccessorReceiver =
		NewObject<UWeaponHostMigrationSubsystem>(GameInstance);
	TestTrue(TEXT("The invalid-successor fixture accepts its baseline"),
		InvalidSuccessorReceiver->AcceptRoleAssignment(Assignment, Error));
	FWeaponMigrationRoleAssignment WrongSuccessor = SuccessorB;
	WrongSuccessor.AuthorityEpoch = 2;
	TestFalse(TEXT("Backup B cannot claim Backup A's old+1 Epoch"),
		InvalidSuccessorReceiver->AcceptRoleAssignment(WrongSuccessor, Error));

	UWeaponHostMigrationSubsystem* CandidateA =
		NewObject<UWeaponHostMigrationSubsystem>(GameInstance);
	TestTrue(TEXT("Candidate A prepares its source term"),
		CandidateA->PrepareAuthorityTerm(MatchId, Host, 17, Epoch, Error));
	TestTrue(TEXT("Candidate A installs its source assignment"),
		CandidateA->SetAuthorityRoleAssignment(Assignment, Error));
	TestTrue(TEXT("Only assigned A can begin old+1"),
		CandidateA->BeginCandidateAuthorityTerm(BackupAId, 1, Epoch, Error));
	TestTrue(TEXT("Candidate A Epoch is old+1"), Epoch == 2);

	UWeaponHostMigrationSubsystem* CandidateB =
		NewObject<UWeaponHostMigrationSubsystem>(GameInstance);
	TestTrue(TEXT("Candidate B prepares its source term"),
		CandidateB->PrepareAuthorityTerm(MatchId, Host, 17, Epoch, Error));
	TestTrue(TEXT("Candidate B installs its source assignment"),
		CandidateB->SetAuthorityRoleAssignment(Assignment, Error));
	TestTrue(TEXT("Only assigned B can begin old+2"),
		CandidateB->BeginCandidateAuthorityTerm(BackupBId, 1, Epoch, Error));
	TestTrue(TEXT("Candidate B Epoch is old+2"), Epoch == 3);

	UWeaponHostMigrationSubsystem* NonCandidate =
		NewObject<UWeaponHostMigrationSubsystem>(GameInstance);
	TestTrue(TEXT("Non-candidate fixture prepares its source term"),
		NonCandidate->PrepareAuthorityTerm(MatchId, Host, 17, Epoch, Error));
	TestTrue(TEXT("Non-candidate fixture installs its source assignment"),
		NonCandidate->SetAuthorityRoleAssignment(Assignment, Error));
	TestFalse(TEXT("The Active Host cannot self-elect as a candidate"),
		NonCandidate->BeginCandidateAuthorityTerm(HostId, 1, Epoch, Error));

	return !HasAnyErrors();
}

IMPLEMENT_SIMPLE_AUTOMATION_TEST(
	FWeaponMigrationCommonAnnouncementAutomationTest,
	"LetsDeepRock.Weapon.HostMigration.CommonAnnouncement",
	EAutomationTestFlags_ApplicationContextMask | EAutomationTestFlags::EngineFilter)

bool FWeaponMigrationCommonAnnouncementAutomationTest::RunTest(const FString& Parameters)
{
	using namespace WeaponHostMigrationTests;

	UGameInstance* GameInstance = FindGameInstance();
	if (!GameInstance)
	{
		AddError(TEXT("A running GameInstance is required; run this test with NewMap loaded"));
		return false;
	}

	const FUniqueNetIdRepl HostId = MakePlayerId(TEXT("1111111111111111"));
	const FUniqueNetIdRepl BackupAId = MakePlayerId(TEXT("2222222222222222"));
	const FUniqueNetIdRepl BackupBId = MakePlayerId(TEXT("3333333333333333"));
	const FUniqueNetIdRepl ObserverId = MakePlayerId(TEXT("4444444444444444"));
	const FGuid MatchId = FGuid::NewGuid();
	FWeaponMigrationRoleAssignment Assignment;
	Assignment.MatchId = MatchId;
	Assignment.AuthorityEpoch = 9;
	Assignment.RosterRevision = 23;
	Assignment.ActiveHost = MakeRoleMember(HostId, 0);
	Assignment.BackupA = MakeRoleMember(BackupAId, 1);
	Assignment.BackupB = MakeRoleMember(BackupBId, 2);

	FString Error;
	UWeaponMatchSaveGame* SaveGame = MakeSaveGame(MatchId, 31, 23, BackupAId, Error);
	if (!TestNotNull(*Error, SaveGame))
	{
		return false;
	}
	TArray<uint8> Payload;
	if (!TestTrue(*Error, UWeaponSnapshotCoordinator::SerializeSaveGame(SaveGame, Payload, Error)))
	{
		return false;
	}

	UWeaponHostMigrationSubsystem* BackupSubsystem =
		NewObject<UWeaponHostMigrationSubsystem>(GameInstance);
	if (!TestTrue(TEXT("Backup accepts the fenced assignment"),
		BackupSubsystem->AcceptRoleAssignment(Assignment, Error)))
	{
		AddError(Error);
		return false;
	}
	FWeaponMigrationPayloadEnvelope Envelope;
	if (!TestTrue(TEXT("The assigned payload envelope is valid"),
		BackupSubsystem->BuildPayloadEnvelope(SaveGame, Payload, 9, Envelope, Error)) ||
		!TestTrue(TEXT("The assigned payload assembles"),
			StoreAllChunks(BackupSubsystem, Envelope, Payload, Error)))
	{
		AddError(Error);
		return false;
	}
	FWeaponMigrationPayloadAck Ack;
	if (!TestTrue(TEXT("The assigned payload promotes locally"),
		BackupSubsystem->PromotePendingPayload(Ack, Error)))
	{
		AddError(Error);
		return false;
	}

	UWeaponHostMigrationSubsystem* EmptyBackup =
		NewObject<UWeaponHostMigrationSubsystem>(GameInstance);
	TestTrue(TEXT("The empty-backup fixture accepts the assignment"),
		EmptyBackup->AcceptRoleAssignment(Assignment, Error));
	TestFalse(
		TEXT("A Backup cannot accept CommonCommitted without identical local bytes"),
		EmptyBackup->AcceptCommonCommittedAnnouncement(BackupAId, Ack, Error));

	UWeaponHostMigrationSubsystem* ObserverSubsystem =
		NewObject<UWeaponHostMigrationSubsystem>(GameInstance);
	TestTrue(TEXT("An observer accepts the fenced assignment"),
		ObserverSubsystem->AcceptRoleAssignment(Assignment, Error));
	TestTrue(TEXT("An observer accepts CommonCommitted metadata without payload bytes"),
		ObserverSubsystem->AcceptCommonCommittedAnnouncement(ObserverId, Ack, Error));
	TestTrue(TEXT("The observer remains outside the confirming Backup A/B roles"),
		ObserverSubsystem->GetRoleForPlayer(ObserverId) == EWeaponMigrationRole::None);
	TArray<uint8> ObserverBytes;
	TestFalse(TEXT("An observer does not gain recovery payload authority"),
		ObserverSubsystem->CopyCommonCommittedPayload(ObserverBytes));

	if (!TestTrue(
		TEXT("A Backup accepts CommonCommitted with identical local bytes"),
		BackupSubsystem->AcceptCommonCommittedAnnouncement(BackupAId, Ack, Error)))
	{
		AddError(Error);
		return false;
	}
	FWeaponMigrationPayloadAck PinnedAck;
	TestTrue(TEXT("The accepted common ACK is pinned"),
		BackupSubsystem->GetLocalCommonCommittedAck(PinnedAck));
	TestTrue(TEXT("The pinned ACK identifies the same payload"),
		PinnedAck.IdentifiesSamePayload(Ack));
	TArray<uint8> CommonBytes;
	TestTrue(TEXT("The common baseline bytes are pinned"),
		BackupSubsystem->CopyCommonCommittedPayload(CommonBytes));
	TestTrue(TEXT("Pinned common bytes are exact"), CommonBytes == Payload);

	FWeaponMigrationPayloadAck StaleEpochAck = Ack;
	--StaleEpochAck.AuthorityEpoch;
	TestFalse(TEXT("A stale Epoch announcement is rejected"),
		BackupSubsystem->AcceptCommonCommittedAnnouncement(BackupAId, StaleEpochAck, Error));
	FWeaponMigrationPayloadAck StaleRosterAck = Ack;
	--StaleRosterAck.RosterRevision;
	TestFalse(TEXT("A stale RosterRevision announcement is rejected"),
		BackupSubsystem->AcceptCommonCommittedAnnouncement(BackupAId, StaleRosterAck, Error));

	return !HasAnyErrors();
}

#endif // WITH_DEV_AUTOMATION_TESTS
