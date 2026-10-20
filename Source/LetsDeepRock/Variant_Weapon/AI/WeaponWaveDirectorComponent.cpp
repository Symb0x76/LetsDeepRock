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

UWeaponWaveDirectorComponent::UWeaponWaveDirectorComponent()
{
	PrimaryComponentTick.bCanEverTick = true;
	PrimaryComponentTick.TickInterval = 0.25f;

	DifficultySpawnerRows[0] = TEXT("EasyWave");
	DifficultySpawnerRows[1] = TEXT("Default");
	DifficultySpawnerRows[2] = TEXT("HardWave");

	CombatWaveEntries =
	{
		{ TEXT("BasicGrunt"), FSoftClassPath(TEXT("/Game/Variant_Weapon/Enemies/BP_Enemy_BasicGrunt.BP_Enemy_BasicGrunt_C")), 0.60f },
		{ TEXT("FastRunner"), FSoftClassPath(TEXT("/Game/Variant_Weapon/Enemies/BP_Enemy_FastRunner.BP_Enemy_FastRunner_C")), 0.25f },
		{ TEXT("Exploder"), FSoftClassPath(TEXT("/Game/Variant_Weapon/Enemies/BP_Enemy_Exploder.BP_Enemy_Exploder_C")), 0.15f },
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

	// 解析敌人 BP 类;软路径失效(资产改名/未同步)时丢弃该条,不阻塞开局。
	ResolvedEnemyClasses.Reset();
	for (const FWeaponEnemyTypeEntry& Entry : CombatWaveEntries)
	{
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
	UE_LOG(LogTemp, Log, TEXT("[A2b] Defense wave started: quota=%d SpawnPoints=%d Difficulty=%d"),
		DefenseWave.RemainingToSpawn, DefenseWave.SpawnPoints.Num(), Difficulty);
	RoomWaves.Add(MoveTemp(DefenseWave));
}

void UWeaponWaveDirectorComponent::ResetWaves(const bool bDestroySpawned)
{
	if (bDestroySpawned)
	{
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
		if (!Room.bTriggered && Room.RemainingToSpawn <= 0)
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

		if (Room.bTriggered && Room.RemainingToSpawn > 0 && Now >= Room.NextSpawnTime)
		{
			SpawnOneEnemy(Room);
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
		TArray<FSpawnerConfigRow> Rows;
		Rows.SetNum(3);
		for (int32 Index = 0; Index < 3; ++Index)
		{
			Rows[Index].SpawnGroupSize = GroupSizes[Index];
			Rows[Index].NPCCap = NPCCaps[Index];
			Rows[Index].SpawnGroupDelay = GroupDelays[Index];
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
	Room.RemainingToSpawn = FMath::Max(1, Row->SpawnGroupSize);
	Room.NextSpawnTime = 0.0;
	UE_LOG(LogTemp, Log, TEXT("[A2] Combat room triggered: quota=%d"), Room.RemainingToSpawn);
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


int32 UWeaponWaveDirectorComponent::PickEnemyEntryIndex() const
{
	float TotalWeight = 0.0f;
	for (int32 Index = 0; Index < CombatWaveEntries.Num(); ++Index)
	{
		if (ResolvedEnemyClasses.IsValidIndex(Index) && ResolvedEnemyClasses[Index])
		{
			TotalWeight += FMath::Max(CombatWaveEntries[Index].Weight, 0.0f);
		}
	}
	if (TotalWeight <= 0.0f)
	{
		return INDEX_NONE;
	}

	float Roll = FMath::FRandRange(0.0f, TotalWeight);
	for (int32 Index = 0; Index < CombatWaveEntries.Num(); ++Index)
	{
		if (ResolvedEnemyClasses.IsValidIndex(Index) && ResolvedEnemyClasses[Index])
		{
			Roll -= FMath::Max(CombatWaveEntries[Index].Weight, 0.0f);
			if (Roll <= 0.0f)
			{
				return Index;
			}
		}
	}
	return INDEX_NONE;
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
