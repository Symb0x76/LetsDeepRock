// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponWaveDirectorComponent.h"

#include "AI/EnemyAIController.h"
#include "Data/GameDataSubsystem.h"
#include "Data/DataTableRows.h"
#include "EnemyCharacter.h"
#include "Engine/GameInstance.h"
#include "EngineUtils.h"
#include "GameFramework/Character.h"
#include "GameFramework/PlayerController.h"
#include "GridMap/GridMapManager.h"
#include "Variant_Weapon/WeaponGameState.h"
#include "Online/WeaponSnapshot.h"
#include "Swarm/SwarmBackend.h"
#include "Swarm/SwarmSubsystem.h"
#include "Swarm/SwarmWaveMath.h"

UWeaponWaveDirectorComponent::UWeaponWaveDirectorComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.25f;

	DifficultySpawnerRows[0] = TEXT("EasyWave");
	DifficultySpawnerRows[1] = TEXT("Default");
	DifficultySpawnerRows[2] = TEXT("HardWave");

	CombatWaveEntries =
	{
		// 虫群档:由 ISwarmBackend 池化生成,不走 BP 类。
		{ TEXT("BasicGrunt"), FSoftClassPath(), 0.60f, true },
		{ TEXT("FastRunner"), FSoftClassPath(), 0.25f, true },
		{ TEXT("Exploder"), FSoftClassPath(), 0.15f, true },
		// 精英档:常规 BP 敌人,各档权重只在本档内比较。
		{ TEXT("HeavyTank"), FSoftClassPath(TEXT("/Game/Variant_Weapon/Enemies/BP_Enemy_HeavyTank.BP_Enemy_HeavyTank_C")), 0.5f, false },
		{ TEXT("Spitter"), FSoftClassPath(TEXT("/Game/Variant_Weapon/Enemies/BP_Enemy_Spitter.BP_Enemy_Spitter_C")), 0.5f, false },
	};
}

void UWeaponWaveDirectorComponent::SetDifficulty(const int32 InDifficulty)
{
	Difficulty = FMath::Clamp(InDifficulty, 1, 3);
}

void UWeaponWaveDirectorComponent::OnMatchStarted(AGridMapManager* Manager)
{
	CachedManager = Manager;
	RoomWaves.Reset();
	EnemySpawnMarkers.Reset();
	SpawnedEnemies.Reset();
	CombatRoomCount = 0;
	bHasDefenseWave = false;
	DefenseCenter = FVector::ZeroVector;

	// 解析敌人 BP 类;软路径失效(资产改名/未同步)时丢弃该条,不阻塞开局。
	ResolvedEnemyClasses.Reset();
	for (const FWeaponEnemyTypeEntry& Entry : CombatWaveEntries)
	{
		if (Entry.bSwarm)
		{
			// 虫群条目不解析类;保持与 CombatWaveEntries 同下标。
			ResolvedEnemyClasses.Add(nullptr);
			continue;
		}
		TSubclassOf<AEnemyCharacter> Resolved =
			Entry.EnemyClassPath.TryLoadClass<AEnemyCharacter>();
		if (!Resolved)
		{
			UE_LOG(LogTemp, Warning,
				TEXT("[A2] Enemy class path unresolved, entry dropped: Row=%s Path=%s"),
				*Entry.EnemyRow.ToString(), *Entry.EnemyClassPath.ToString());
		}
		ResolvedEnemyClasses.Add(Resolved);
	}

	if (!Manager)
	{
		return;
	}

	// Combat 房间边界 + 全体 EnemySpawn marker(按房间 2D 盒过滤归属)。
	TArray<FBox2D> CombatBounds;
	Manager->GetRoomWorldBoundsByPurpose(EGridMapRoomPurpose::Combat, CombatBounds);
	Manager->GetMarkerWorldLocations(EGridMapMarkerType::EnemySpawn, EnemySpawnMarkers);

	for (const FBox2D& Bounds : CombatBounds)
	{
		FRoomWave& Room = RoomWaves.AddDefaulted_GetRef();
		Room.Bounds = Bounds;

		// 房间内 marker 优先;不足时补房间中心附近散点,保证每个 Combat 房都能出怪。
		for (const FVector& Marker : EnemySpawnMarkers)
		{
			if (Bounds.IsInside(FVector2D(Marker.X, Marker.Y)))
			{
				Room.SpawnPoints.Add(Marker);
				if (Room.SpawnPoints.Num() >= MaxCachedSpawnPoints)
				{
					break;
				}
			}
		}
		if (Room.SpawnPoints.IsEmpty())
		{
			const FVector2D Center = Bounds.GetCenter();
			Room.SpawnPoints.Add(FVector(Center.X, Center.Y, 0.0f));
		}
	}

	CombatRoomCount = RoomWaves.Num();

	const UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	const UGameDataSubsystem* DataSubsystem =
		GameInstance ? GameInstance->GetSubsystem<UGameDataSubsystem>() : nullptr;
	if (!DataSubsystem || !DataSubsystem->GetSpawnerConfig(DifficultySpawnerRows[Difficulty - 1]))
	{
		UE_LOG(LogTemp, Warning,
			TEXT("[A2] Spawner row '%s' not found (DT_SpawnerConfig missing?); using built-in tier %d defaults"),
			*DifficultySpawnerRows[Difficulty - 1].ToString(), Difficulty);
	}

	UE_LOG(LogTemp, Log,
		TEXT("[A2] Wave director armed:CombatRooms=%d EnemyMarkers=%d ResolvedClasses=%d Difficulty=%d"),
		RoomWaves.Num(), EnemySpawnMarkers.Num(),
		static_cast<int32>(ResolvedEnemyClasses.Num()), Difficulty);
}

void UWeaponWaveDirectorComponent::StartDefenseWave(const FVector& Center)
{
	if (!GetOwner() || !GetOwner()->HasAuthority())
	{
		return;
	}

	// 防御波复用房间波次的滴流/上限/散布逻辑:一个"已触发"的虚拟房间,刷怪点取离撤离中心
	// 最近的 EnemySpawn marker(跳过过近点),敌人从周边房间压向撤离区。
	FRoomWave DefenseWave;
	DefenseWave.Bounds = FBox2D(FVector2D(Center.X, Center.Y), FVector2D(Center.X, Center.Y));

	const float MinDistanceSquared = FMath::Square(DefenseMinSpawnDistance);
	TArray<FVector> Candidates;
	for (const FVector& Marker : EnemySpawnMarkers)
	{
		if (FVector::DistSquared2D(Marker, Center) >= MinDistanceSquared)
		{
			Candidates.Add(Marker);
		}
	}
	Candidates.Sort([&Center](const FVector& A, const FVector& B)
	{
		return FVector::DistSquared2D(A, Center) < FVector::DistSquared2D(B, Center);
	});
	for (int32 Index = 0; Index < Candidates.Num() && Index < MaxCachedSpawnPoints; ++Index)
	{
		DefenseWave.SpawnPoints.Add(Candidates[Index]);
	}
	if (DefenseWave.SpawnPoints.IsEmpty())
	{
		// 没有可用 marker:退回撤离中心外一圈(PickSpawnLocation 之后仍会按行配置散布)。
		DefenseWave.SpawnPoints.Add(Center + FVector(DefenseMinSpawnDistance, 0.0f, 0.0f));
	}

	TriggerRoom(DefenseWave);
	if (!DefenseWave.bTriggered)
	{
		return;
	}
	UE_LOG(LogTemp, Log, TEXT("[A2b] Defense wave started: swarm=%d elites=%d SpawnPoints=%d Difficulty=%d"),
		DefenseWave.RemainingSwarm, DefenseWave.RemainingToSpawn, DefenseWave.SpawnPoints.Num(), Difficulty);
	RoomWaves.Add(MoveTemp(DefenseWave));
	bHasDefenseWave = true;
	DefenseCenter = Center;
}

void UWeaponWaveDirectorComponent::CaptureSnapshotState(FWeaponWaveDirectorSnapshot& OutSnapshot) const
{
	OutSnapshot = FWeaponWaveDirectorSnapshot();
	const UWorld* World = GetWorld();
	if (!World || RoomWaves.IsEmpty())
	{
		return;
	}

	const double Now = World->GetTimeSeconds();
	auto ToSnapshot = [Now](const FRoomWave& Room)
	{
		FWeaponWaveRoomSnapshot Saved;
		Saved.bTriggered = Room.bTriggered;
		Saved.RemainingToSpawn = Room.RemainingToSpawn;
		Saved.RemainingSwarm = Room.RemainingSwarm;
		Saved.SecondsToNextSpawn = static_cast<float>(FMath::Max(0.0, Room.NextSpawnTime - Now));
		return Saved;
	};

	OutSnapshot.bArmed = true;
	OutSnapshot.Difficulty = Difficulty;
	for (int32 Index = 0; Index < CombatRoomCount && RoomWaves.IsValidIndex(Index); ++Index)
	{
		OutSnapshot.CombatRooms.Add(ToSnapshot(RoomWaves[Index]));
	}
	if (bHasDefenseWave && RoomWaves.IsValidIndex(CombatRoomCount))
	{
		OutSnapshot.bHasDefenseWave = true;
		OutSnapshot.DefenseCenter = DefenseCenter;
		OutSnapshot.DefenseWave = ToSnapshot(RoomWaves[CombatRoomCount]);
	}

	// 存活虫群按房间归并(RoomWaves 顺序:战斗房在前,防御波在 CombatRoomCount)。
	if (const USwarmSubsystem* Swarm = USwarmSubsystem::Get(this))
	{
		if (const ISwarmBackend* Backend = Swarm->GetBackend())
		{
			TArray<FSwarmBugView> Bugs;
			Backend->ForEachAlive([&Bugs](const FSwarmBugView& View) { Bugs.Add(View); });
			TArray<FBox2D> Rooms;
			Rooms.Reserve(RoomWaves.Num());
			for (const FRoomWave& Room : RoomWaves)
			{
				Rooms.Add(Room.Bounds);
			}
			OutSnapshot.SwarmCounts = SwarmBucketByRoom(Bugs, Rooms);
		}
	}
}

void UWeaponWaveDirectorComponent::RestoreSnapshotState(AGridMapManager* Manager, const FWeaponWaveDirectorSnapshot& Snapshot)
{
	UWorld* World = GetWorld();
	if (!World || !Snapshot.bArmed)
	{
		return;
	}

	SetDifficulty(Snapshot.Difficulty);
	// 房间边界与刷怪点由 Recipe 决定,恢复世界的 Recipe 与原世界一致,重建后顺序相同。
	OnMatchStarted(Manager);
	if (CombatRoomCount != Snapshot.CombatRooms.Num())
	{
		UE_LOG(LogTemp, Warning, TEXT("[A2] Restored combat room count %d differs from snapshot %d; applying the overlap"),
			CombatRoomCount, Snapshot.CombatRooms.Num());
	}

	const double Now = World->GetTimeSeconds();
	auto Apply = [Now](FRoomWave& Room, const FWeaponWaveRoomSnapshot& Saved)
	{
		Room.bTriggered = Saved.bTriggered;
		Room.RemainingToSpawn = FMath::Max(0, Saved.RemainingToSpawn);
		Room.RemainingSwarm = FMath::Max(0, Saved.RemainingSwarm);
		Room.NextSpawnTime = Now + FMath::Max(0.0f, Saved.SecondsToNextSpawn);
	};
	for (int32 Index = 0; Index < CombatRoomCount && Snapshot.CombatRooms.IsValidIndex(Index); ++Index)
	{
		Apply(RoomWaves[Index], Snapshot.CombatRooms[Index]);
	}
	if (Snapshot.bHasDefenseWave)
	{
		StartDefenseWave(Snapshot.DefenseCenter);
		if (bHasDefenseWave && RoomWaves.IsValidIndex(CombatRoomCount))
		{
			Apply(RoomWaves[CombatRoomCount], Snapshot.DefenseWave);
		}
	}

	// 虫群在房间与防御波都重建之后再按计数重新生成(RoomIndex == CombatRoomCount 此时才有效)。
	if (!Snapshot.SwarmCounts.IsEmpty())
	{
		const USwarmSubsystem* Swarm = USwarmSubsystem::Get(this);
		ISwarmBackend* Backend = Swarm ? Swarm->GetBackend() : nullptr;
		if (Backend)
		{
			// 先清空现存小虫,避免与快照计数叠加。
			Backend->DespawnAll();
			int32 Requested = 0;
			int32 Spawned = 0;
			for (const FWeaponSwarmRoomCount& Entry : Snapshot.SwarmCounts)
			{
				if (Entry.Count <= 0 || !RoomWaves.IsValidIndex(Entry.RoomIndex))
				{
					UE_LOG(LogTemp, Log, TEXT("[Swarm] Restore skipped invalid swarm entry: room %d row %s count %d"),
						Entry.RoomIndex, *Entry.ArchetypeRow.ToString(), Entry.Count);
					continue;
				}
				Requested += Entry.Count;
				Spawned += Backend->SpawnAt(Entry.ArchetypeRow, PickSpawnLocation(RoomWaves[Entry.RoomIndex]), Entry.Count);
			}
			if (Spawned < Requested)
			{
				UE_LOG(LogTemp, Warning, TEXT("[A2] Swarm restore clamped: requested %d spawned %d"), Requested, Spawned);
			}
		}
		else
		{
			UE_LOG(LogTemp, Warning, TEXT("[Swarm] No swarm backend on restore; dropping %d swarm entries"), Snapshot.SwarmCounts.Num());
		}
	}

	// 快照恢复出的敌人不是本组件刷出的;接管它们,使 NPC 上限与局末清理照常生效。
	for (TActorIterator<AEnemyCharacter> It(World); It; ++It)
	{
		SpawnedEnemies.Add(*It);
	}
}

void UWeaponWaveDirectorComponent::ResetWaves(const bool bDestroySpawned)
{
	if (bDestroySpawned)
	{
		if (const USwarmSubsystem* Swarm = USwarmSubsystem::Get(this))
		{
			if (ISwarmBackend* Backend = Swarm->GetBackend())
			{
				Backend->DespawnAll();
			}
		}
		for (const TWeakObjectPtr<AEnemyCharacter>& Enemy : SpawnedEnemies)
		{
			if (Enemy.IsValid())
			{
				Enemy->Destroy();
			}
		}
	}
	SpawnedEnemies.Reset();
	for (FRoomWave& Room : RoomWaves)
	{
		Room.bTriggered = false;
		Room.RemainingToSpawn = 0;
		Room.RemainingSwarm = 0;
		Room.NextSpawnTime = 0.0;
	}
}

void UWeaponWaveDirectorComponent::TickComponent(
	const float DeltaTime, const ELevelTick TickType, FActorComponentTickFunction* ThisTickFunction)
{
	Super::TickComponent(DeltaTime, TickType, ThisTickFunction);

	UWorld* World = GetWorld();
	if (!World || !GetOwner()->HasAuthority() ||
		AWeaponGameState::IsGameplayRecoveryBlockedFor(this))
	{
		return;
	}

	const double Now = World->GetTimeSeconds();

	// 未触发的房间:任一玩家 Pawn 进入 2D 边界即触发(只遍历 PlayerController,追击的敌人不触发别的房)。
	for (FRoomWave& Room : RoomWaves)
	{
		if (!Room.bTriggered && Room.RemainingToSpawn <= 0 && Room.RemainingSwarm <= 0)
		{
			for (FConstPlayerControllerIterator It = World->GetPlayerControllerIterator(); It; ++It)
			{
				const AController* Controller = It->Get();
				const APawn* Pawn = Controller ? Controller->GetPawn() : nullptr;
				if (Pawn && Room.Bounds.IsInside(FVector2D(
						Pawn->GetActorLocation().X, Pawn->GetActorLocation().Y)))
				{
					TriggerRoom(Room);
					break;
				}
			}
		}

		if (Room.bTriggered && (Room.RemainingSwarm > 0 || Room.RemainingToSpawn > 0) && Now >= Room.NextSpawnTime)
		{
			// 虫群爆发与精英各自排程,取较早者作为下次到期时间(哨兵 Max 表示该分支未排程)。
			constexpr double Unscheduled = TNumericLimits<double>::Max();
			double NextDue = Unscheduled;
			if (Room.RemainingSwarm > 0)
			{
				Room.NextSpawnTime = Unscheduled;
				SpawnSwarmBurst(Room);
				NextDue = FMath::Min(NextDue, Room.NextSpawnTime);
			}
			if (Room.RemainingToSpawn > 0)
			{
				Room.NextSpawnTime = Unscheduled;
				SpawnOneEnemy(Room);
				NextDue = FMath::Min(NextDue, Room.NextSpawnTime);
			}
			Room.NextSpawnTime = NextDue < Unscheduled ? NextDue : Now + 0.25;
		}
	}
}

const FSpawnerConfigRow* UWeaponWaveDirectorComponent::GetSpawnerRow() const
{
	const UGameInstance* GameInstance = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	const UGameDataSubsystem* DataSubsystem =
		GameInstance ? GameInstance->GetSubsystem<UGameDataSubsystem>() : nullptr;
	if (const FSpawnerConfigRow* Row =
		DataSubsystem ? DataSubsystem->GetSpawnerConfig(DifficultySpawnerRows[Difficulty - 1]) : nullptr)
	{
		return Row;
	}

	// DT_SpawnerConfig(Content/P4)缺失或缺行时退回内置三档,保证白盒仍有波次;配表到位后自动以表为准。
	static const TArray<FSpawnerConfigRow> FallbackRows = []
	{
		const int32 GroupSizes[3] = { 3, 5, 8 };
		const int32 NPCCaps[3] = { 10, 16, 24 };
		const float GroupDelays[3] = { 6.0f, 5.0f, 4.0f };
		const int32 SwarmQuotas[3] = { 40, 60, 90 };
		const int32 SwarmBursts[3] = { 6, 8, 10 };
		const int32 SwarmCaps[3] = { 80, 120, 150 };
		const int32 EliteQuotas[3] = { 1, 2, 3 };
		TArray<FSpawnerConfigRow> Rows;
		Rows.SetNum(3);
		for (int32 Index = 0; Index < 3; ++Index)
		{
			Rows[Index].SpawnGroupSize = GroupSizes[Index];
			Rows[Index].NPCCap = NPCCaps[Index];
			Rows[Index].SpawnGroupDelay = GroupDelays[Index];
			Rows[Index].SwarmQuotaPerRoom = SwarmQuotas[Index];
			Rows[Index].SwarmBurstSize = SwarmBursts[Index];
			Rows[Index].SwarmCap = SwarmCaps[Index];
			Rows[Index].EliteQuotaPerRoom = EliteQuotas[Index];
		}
		return Rows;
	}();
	return &FallbackRows[FMath::Clamp(Difficulty, 1, 3) - 1];
}

void UWeaponWaveDirectorComponent::TriggerRoom(FRoomWave& Room)
{
	const FSpawnerConfigRow* Row = GetSpawnerRow();
	if (!Row)
	{
		return;
	}
	Room.bTriggered = true;
	// 虫群走 RemainingSwarm 爆发;精英复用 RemainingToSpawn(SpawnOneEnemy,受 NPCCap 限制)。
	Room.RemainingSwarm = FMath::Max(0, Row->SwarmQuotaPerRoom);
	Room.RemainingToSpawn = FMath::Max(0, Row->EliteQuotaPerRoom);
	Room.NextSpawnTime = 0.0;
	UE_LOG(LogTemp, Log, TEXT("[A2] Combat room triggered: swarm=%d elites=%d"), Room.RemainingSwarm, Room.RemainingToSpawn);
}

void UWeaponWaveDirectorComponent::SpawnOneEnemy(FRoomWave& Room)
{
	UWorld* World = GetWorld();
	const FSpawnerConfigRow* Row = GetSpawnerRow();
	if (!World || !Row)
	{
		return;
	}

	// NPC 上限:在册存活数达 cap 则顺延(不吞掉配额)。
	int32 AliveCount = 0;
	for (const TWeakObjectPtr<AEnemyCharacter>& Enemy : SpawnedEnemies)
	{
		if (Enemy.IsValid())
		{
			++AliveCount;
		}
	}
	if (AliveCount >= Row->NPCCap)
	{
		Room.NextSpawnTime = World->GetTimeSeconds() + Row->SpawnGroupDelay;
		return;
	}

	const int32 EntryIndex = PickEnemyEntryIndex();
	if (EntryIndex == INDEX_NONE)
	{
		Room.RemainingToSpawn = 0;
		return;
	}

	FVector Location = PickSpawnLocation(Room);
	const float Angle = FMath::FRandRange(0.0f, 2.0f * PI);
	const float Offset = FMath::FRandRange(0.0f, Row->SpawnRadius);
	Location.X += FMath::Cos(Angle) * Offset;
	Location.Y += FMath::Sin(Angle) * Offset;
	Location.Z += 90.0f;

	const FRotator SpawnRotation(0.0f, FMath::FRandRange(0.0f, 360.0f), 0.0f);
	AEnemyCharacter* Enemy = World->SpawnActorDeferred<AEnemyCharacter>(
		ResolvedEnemyClasses[EntryIndex],
		FTransform(SpawnRotation, Location),
		nullptr, nullptr, ESpawnActorCollisionHandlingMethod::AlwaysSpawn);
	if (Enemy)
	{
		Enemy->SetEnemyConfigRowName(CombatWaveEntries[EntryIndex].EnemyRow);
		Enemy->FinishSpawning(FTransform(SpawnRotation, Location));
		SpawnedEnemies.Add(Enemy);
		--Room.RemainingToSpawn;
	}

	// 个体间隔在行配置的 [Min, Max] 内随机。
	const float Delay = FMath::FRandRange(Row->MinSpawnDelay, Row->MaxSpawnDelay);
	Room.NextSpawnTime = World->GetTimeSeconds() + FMath::Max(Delay, 0.05f);
}

void UWeaponWaveDirectorComponent::SpawnSwarmBurst(FRoomWave& Room)
{
	UWorld* World = GetWorld();
	const FSpawnerConfigRow* Row = GetSpawnerRow();
	if (!World || !Row)
	{
		return;
	}

	const USwarmSubsystem* Swarm = USwarmSubsystem::Get(this);
	ISwarmBackend* Backend = Swarm ? Swarm->GetBackend() : nullptr;
	if (!Backend)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Swarm] No swarm backend; skipping swarm quota for this room"));
		Room.RemainingSwarm = 0;
		return;
	}

	const int32 Burst = SwarmComputeBurst(Room.RemainingSwarm, Backend->CountAlive(), Row->SwarmCap, Row->SwarmBurstSize);
	if (Burst <= 0)
	{
		// 存活数已达上限:顺延,不吞配额。
		Room.NextSpawnTime = World->GetTimeSeconds() + Row->SpawnGroupDelay;
		return;
	}

	const FName ArchetypeRow = PickSwarmRow();
	if (ArchetypeRow.IsNone())
	{
		UE_LOG(LogTemp, Warning, TEXT("[Swarm] No swarm entries in CombatWaveEntries; skipping swarm quota"));
		Room.RemainingSwarm = 0;
		return;
	}

	// 只扣实际生成数(池满时可能少于 Burst)。
	const int32 Spawned = Backend->SpawnAt(ArchetypeRow, PickSpawnLocation(Room), Burst);
	Room.RemainingSwarm = FMath::Max(0, Room.RemainingSwarm - Spawned);
	if (Spawned <= 0)
	{
		// 池耗尽:按群间隔顺延,不走短随机间隔空转。
		Room.NextSpawnTime = World->GetTimeSeconds() + Row->SpawnGroupDelay;
		return;
	}

	const float Delay = FMath::FRandRange(Row->MinSpawnDelay, Row->MaxSpawnDelay);
	Room.NextSpawnTime = World->GetTimeSeconds() + FMath::Max(Delay, 0.05f);
}

FName UWeaponWaveDirectorComponent::PickSwarmRow() const
{
	float TotalWeight = 0.0f;
	for (const FWeaponEnemyTypeEntry& Entry : CombatWaveEntries)
	{
		if (Entry.bSwarm)
		{
			TotalWeight += FMath::Max(Entry.Weight, 0.0f);
		}
	}
	if (TotalWeight <= 0.0f)
	{
		return NAME_None;
	}

	float Roll = FMath::FRandRange(0.0f, TotalWeight);
	FName Last = NAME_None;
	for (const FWeaponEnemyTypeEntry& Entry : CombatWaveEntries)
	{
		if (Entry.bSwarm && Entry.Weight > 0.0f)
		{
			Last = Entry.EnemyRow;
			Roll -= Entry.Weight;
			if (Roll <= 0.0f)
			{
				return Entry.EnemyRow;
			}
		}
	}
	// FRandRange 可能恰好返回总权重:落到最后一个正权重行。
	return Last;
}

int32 UWeaponWaveDirectorComponent::PickEnemyEntryIndex() const
{
	float TotalWeight = 0.0f;
	for (int32 Index = 0; Index < CombatWaveEntries.Num(); ++Index)
	{
		if (!CombatWaveEntries[Index].bSwarm && ResolvedEnemyClasses.IsValidIndex(Index) && ResolvedEnemyClasses[Index])
		{
			TotalWeight += FMath::Max(CombatWaveEntries[Index].Weight, 0.0f);
		}
	}
	if (TotalWeight <= 0.0f)
	{
		return INDEX_NONE;
	}

	float Roll = FMath::FRandRange(0.0f, TotalWeight);
	int32 LastIndex = INDEX_NONE;
	for (int32 Index = 0; Index < CombatWaveEntries.Num(); ++Index)
	{
		if (!CombatWaveEntries[Index].bSwarm && CombatWaveEntries[Index].Weight > 0.0f &&
			ResolvedEnemyClasses.IsValidIndex(Index) && ResolvedEnemyClasses[Index])
		{
			LastIndex = Index;
			Roll -= CombatWaveEntries[Index].Weight;
			if (Roll <= 0.0f)
			{
				return Index;
			}
		}
	}
	// FRandRange 可能恰好返回总权重:落到最后一个正权重条目。
	return LastIndex;
}

FVector UWeaponWaveDirectorComponent::PickSpawnLocation(const FRoomWave& Room) const
{
	if (Room.SpawnPoints.IsEmpty())
	{
		const FVector2D Center = Room.Bounds.GetCenter();
		return FVector(Center.X, Center.Y, 0.0f);
	}
	return Room.SpawnPoints[FMath::RandRange(0, Room.SpawnPoints.Num() - 1)];
}
