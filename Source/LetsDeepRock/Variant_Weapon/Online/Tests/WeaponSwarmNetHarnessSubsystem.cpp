// Swarm 多进程网络冒烟：命令行驱动的 GameInstance 子系统（不进入 Shipping）

#include "Online/Tests/WeaponSwarmNetHarnessSubsystem.h"

#include "AbilitySystemComponent.h"
#include "AI/WeaponWaveDirectorComponent.h"
#include "Attributes/WeaponHealthSet.h"
#include "Engine/NetConnection.h"
#include "Engine/NetDriver.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/GameModeBase.h"
#include "GameFramework/GameStateBase.h"
#include "GameFramework/Pawn.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/DateTime.h"
#include "Misc/Parse.h"
#include "Swarm/SwarmBackend.h"
#include "Swarm/SwarmBugPawn.h"
#include "Swarm/SwarmSubsystem.h"
#include "WeaponPlayerState.h"

DEFINE_LOG_CATEGORY_STATIC(LogWeaponSwarmHarness, Log, All);

namespace WeaponSwarmNetHarness
{
	/** 0 = 每帧 Tick；滑行检测需要逐帧位移 */
	constexpr float TickIntervalSeconds = 0.f;
	constexpr double FindRetrySeconds = 1.0;
	constexpr int32 MaxFindResults = 50;
	constexpr int32 MaxPlayers = 4;
	const FName GruntRow(TEXT("BasicGrunt"));
	constexpr int32 SpawnCount = 30;
	/** 生成成功的最低数量（与脚本断言阈值一致） */
	constexpr int32 MinAcceptedSpawn = 27;
	/** 玩家全部到位后再等待的秒数，让客户端完成初始复制 */
	constexpr double SpawnSettleSeconds = 2.0;
	constexpr float NearSpawnMinDistance = 500.f;
	constexpr float NearSpawnMaxDistance = 900.f;
	constexpr float RecycleMinDistance = 3300.f;
	constexpr float RecycleMaxDistance = 4200.f;
	/** 回收后实际虫位置到主机 Pawn 的最小距离要求 */
	constexpr float RecycleRequiredDistance = 3000.f;
	constexpr float InvulnerableHealth = 1000000.f;
	/** 单帧位移超过该值视为"快速帧"；连续 SlideFrameThreshold 帧即计一次滑行 */
	constexpr float SlideFrameDistance = 150.f;
	constexpr int32 SlideFrameThreshold = 3;
	const TCHAR* DefaultMap = TEXT("/Game/GridMap/Maps/L_GridMapStage4Test");
}

namespace
{
	int64 SwarmHarnessUnixMillis()
	{
		static const FDateTime Epoch(1970, 1, 1);
		return (FDateTime::UtcNow() - Epoch).GetTicks() / ETimespan::TicksPerMillisecond;
	}

	const TCHAR* SwarmHarnessLexNetMode(const ENetMode NetMode)
	{
		switch (NetMode)
		{
		case NM_Standalone: return TEXT("Standalone");
		case NM_DedicatedServer: return TEXT("DedicatedServer");
		case NM_ListenServer: return TEXT("ListenServer");
		case NM_Client: return TEXT("Client");
		default: return TEXT("Unknown");
		}
	}

	APawn* SwarmHarnessLocalPawn(const UGameInstance& GameInstance, UWorld& World)
	{
		const APlayerController* Controller = GameInstance.GetFirstLocalPlayerController(&World);
		return Controller ? Controller->GetPawn() : nullptr;
	}

	/** 候选点下方 1000uu 内有 WorldStatic 地面才可用，避免把虫生成到地图外的虚空 */
	bool SwarmHarnessHasGround(const UWorld& World, const FVector& Point)
	{
		FHitResult Hit;
		return World.LineTraceSingleByObjectType(Hit, Point + FVector(0.0, 0.0, 200.0),
			Point - FVector(0.0, 0.0, 1000.0), FCollisionObjectQueryParams(ECC_WorldStatic));
	}
}

bool UWeaponSwarmNetHarnessSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
#if UE_BUILD_SHIPPING
	return false;
#else
	FString RequestedRole;
	return Super::ShouldCreateSubsystem(Outer) &&
		FParse::Value(FCommandLine::Get(), TEXT("WeaponSwarmHarnessRole="), RequestedRole) &&
		!RequestedRole.IsEmpty();
#endif
}

void UWeaponSwarmNetHarnessSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Collection.InitializeDependency<UWeaponSessionSubsystem>();

	FString RequestedRole;
	FParse::Value(FCommandLine::Get(), TEXT("WeaponSwarmHarnessRole="), RequestedRole);
	if (!FParse::Value(FCommandLine::Get(), TEXT("WeaponSwarmHarnessRun="), RunId))
	{
		RunId = TEXT("Manual");
	}
	if (!FParse::Value(FCommandLine::Get(), TEXT("WeaponSwarmHarnessLabel="), ProcessLabel))
	{
		ProcessLabel = RequestedRole;
	}
	RunId = SanitizeMarkerValue(MoveTemp(RunId));
	ProcessLabel = SanitizeMarkerValue(MoveTemp(ProcessLabel));
	ServerName = FString::Printf(TEXT("WSH_%s"), *RunId);

	FString MapOverride;
	HostMapName = FParse::Value(FCommandLine::Get(), TEXT("WeaponSwarmHarnessMap="), MapOverride)
		? FName(*MapOverride)
		: FName(WeaponSwarmNetHarness::DefaultMap);
	FParse::Value(FCommandLine::Get(), TEXT("WeaponSwarmHarnessMinPlayers="), MinPlayersToSpawn);
	FParse::Value(FCommandLine::Get(), TEXT("WeaponSwarmHarnessRecycleDelay="), RecycleDelaySeconds);

	if (RequestedRole.Equals(TEXT("Host"), ESearchCase::IgnoreCase))
	{
		ProcessRole = EProcessRole::Host;
	}
	else if (RequestedRole.Equals(TEXT("Client"), ESearchCase::IgnoreCase))
	{
		ProcessRole = EProcessRole::Client;
	}
	else
	{
		UE_LOG(LogWeaponSwarmHarness, Error, TEXT("[Swarm] Invalid -WeaponSwarmHarnessRole=%s"), *RequestedRole);
		return;
	}

	UWeaponSessionSubsystem* SessionSubsystem = GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	if (!SessionSubsystem)
	{
		EmitMarker(TEXT("DriverFailed"), TEXT("Reason=SessionSubsystemUnavailable"));
		return;
	}
	SessionSubsystem->OnHostComplete.AddDynamic(this, &UWeaponSwarmNetHarnessSubsystem::HandleHostComplete);
	SessionSubsystem->OnFindComplete.AddDynamic(this, &UWeaponSwarmNetHarnessSubsystem::HandleFindComplete);
	SessionSubsystem->OnJoinComplete.AddDynamic(this, &UWeaponSwarmNetHarnessSubsystem::HandleJoinComplete);

	TickerHandle = FTSTicker::GetCoreTicker().AddTicker(
		FTickerDelegate::CreateUObject(this, &UWeaponSwarmNetHarnessSubsystem::TickHarness),
		WeaponSwarmNetHarness::TickIntervalSeconds);
	EmitMarker(TEXT("DriverReady"), FString::Printf(TEXT("Role=%s|Server=%s|Map=%s"),
		*RequestedRole, *ServerName, *HostMapName.ToString()));
}

void UWeaponSwarmNetHarnessSubsystem::Deinitialize()
{
	if (TickerHandle.IsValid())
	{
		FTSTicker::RemoveTicker(TickerHandle);
		TickerHandle.Reset();
	}
	if (UWeaponSessionSubsystem* SessionSubsystem = GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>())
	{
		SessionSubsystem->OnHostComplete.RemoveDynamic(this, &UWeaponSwarmNetHarnessSubsystem::HandleHostComplete);
		SessionSubsystem->OnFindComplete.RemoveDynamic(this, &UWeaponSwarmNetHarnessSubsystem::HandleFindComplete);
		SessionSubsystem->OnJoinComplete.RemoveDynamic(this, &UWeaponSwarmNetHarnessSubsystem::HandleJoinComplete);
	}
	Super::Deinitialize();
}

bool UWeaponSwarmNetHarnessSubsystem::TickHarness(const float DeltaSeconds)
{
	(void)DeltaSeconds;
	UWorld* World = GetWorld();
	if (!World || !World->IsGameWorld())
	{
		return true;
	}
	DriveSession(*World);
	if (ProcessRole == EProcessRole::Host)
	{
		TickHost(*World);
	}
	else if (ProcessRole == EProcessRole::Client)
	{
		TickClient(*World);
	}
	return true;
}

void UWeaponSwarmNetHarnessSubsystem::DriveSession(UWorld& World)
{
	UWeaponSessionSubsystem* SessionSubsystem = GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	if (!SessionSubsystem || SessionSubsystem->GetCurrentOperation() != EWeaponSessionOperation::Idle ||
		World.GetNetMode() != NM_Standalone)
	{
		return;
	}

	if (ProcessRole == EProcessRole::Host)
	{
		if (!bHostRequestStarted)
		{
			bHostRequestStarted = SessionSubsystem->HostLanGameOnMap(
				WeaponSwarmNetHarness::MaxPlayers, ServerName, HostMapName);
			EmitMarker(bHostRequestStarted ? TEXT("HostRequested") : TEXT("HostRequestRejected"),
				FString::Printf(TEXT("Map=%s"), *HostMapName.ToString()));
		}
		return;
	}

	if (bJoinStarted || bFindInFlight || FPlatformTime::Seconds() < NextFindAttemptSeconds)
	{
		return;
	}
	bFindInFlight = SessionSubsystem->FindLanGames(WeaponSwarmNetHarness::MaxFindResults);
	NextFindAttemptSeconds = FPlatformTime::Seconds() + WeaponSwarmNetHarness::FindRetrySeconds;
	EmitMarker(bFindInFlight ? TEXT("FindRequested") : TEXT("FindRequestRejected"));
}

void UWeaponSwarmNetHarnessSubsystem::TickHost(UWorld& World)
{
	if (World.GetNetMode() != NM_ListenServer)
	{
		return;
	}
	USwarmSubsystem* Swarm = USwarmSubsystem::Get(&World);
	ISwarmBackend* Backend = Swarm ? Swarm->GetBackend() : nullptr;
	APawn* HostPawn = SwarmHarnessLocalPawn(*GetGameInstance(), World);
	if (!Backend || !HostPawn)
	{
		return;
	}

	MakePlayersInvulnerable(World);

	if (!bHostReadyEmitted)
	{
		bHostReadyEmitted = true;
		// 停波次导演，避免房间触发的虫群/精英干扰计数与平均位置
		if (AGameModeBase* GameMode = World.GetAuthGameMode())
		{
			if (UWeaponWaveDirectorComponent* Director = GameMode->FindComponentByClass<UWeaponWaveDirectorComponent>())
			{
				Director->SetComponentTickEnabled(false);
			}
		}
		Backend->DespawnAll();
		EmitMarker(TEXT("HostReady"), FString::Printf(TEXT("Map=%s|Capacity=%d"),
			*World.GetOutermost()->GetName(), Backend->GetCapacity()));
	}

	// 玩家数：仅统计已有 Pawn 的玩家，保证虫子有目标且客户端已进入世界
	int32 PlayersWithPawn = 0;
	if (const AGameStateBase* GameState = World.GetGameState())
	{
		for (const APlayerState* PlayerState : GameState->PlayerArray)
		{
			PlayersWithPawn += (PlayerState && PlayerState->GetPawn()) ? 1 : 0;
		}
	}
	if (PlayersWithPawn != LastPlayerCount)
	{
		EmitMarker(TEXT("PlayerCount"), FString::Printf(TEXT("Players=%d|Previous=%d"), PlayersWithPawn, LastPlayerCount));
		LastPlayerCount = PlayersWithPawn;
	}

	const double Now = FPlatformTime::Seconds();
	if (!bSpawned)
	{
		if (PlayersWithPawn < MinPlayersToSpawn)
		{
			PlayersReadySeconds = 0.0;
		}
		else if (PlayersReadySeconds <= 0.0)
		{
			PlayersReadySeconds = Now;
		}
		else if (Now - PlayersReadySeconds >= WeaponSwarmNetHarness::SpawnSettleSeconds)
		{
			int32 Spawned = 0;
			FVector Center = FVector::ZeroVector;
			const bool bOk = SpawnGruntsNear(World, HostPawn->GetActorLocation(),
				WeaponSwarmNetHarness::NearSpawnMinDistance, WeaponSwarmNetHarness::NearSpawnMaxDistance, Spawned, Center);
			bSpawned = true;
			SpawnedSeconds = Now;
			EmitMarker(bOk ? TEXT("SPAWNED") : TEXT("SPAWN_FAILED"),
				FString::Printf(TEXT("Spawned=%d|Alive=%d|CenterX=%.0f|CenterY=%.0f"),
					Spawned, Backend->CountAlive(), Center.X, Center.Y));
		}
	}
	else if (!bRecycled && Now - SpawnedSeconds >= RecycleDelaySeconds)
	{
		bRecycled = true;
		Backend->DespawnAll();
		int32 Spawned = 0;
		FVector Center = FVector::ZeroVector;
		const FVector HostLocation = HostPawn->GetActorLocation();
		const bool bOk = SpawnGruntsNear(World, HostLocation,
			WeaponSwarmNetHarness::RecycleMinDistance, WeaponSwarmNetHarness::RecycleMaxDistance, Spawned, Center);
		float MinDistance = TNumericLimits<float>::Max();
		Backend->ForEachAlive([&MinDistance, &HostLocation](const FSwarmBugView& View)
		{
			MinDistance = FMath::Min(MinDistance, static_cast<float>(FVector::Dist2D(View.Location, HostLocation)));
		});
		EmitMarker(bOk ? TEXT("RECYCLED") : TEXT("RECYCLE_FAILED"),
			FString::Printf(TEXT("Spawned=%d|Alive=%d|CenterX=%.0f|CenterY=%.0f|MinDist=%.0f"),
				Spawned, Backend->CountAlive(), Center.X, Center.Y, Spawned > 0 ? MinDistance : 0.f));
	}

	const int64 Second = FDateTime::UtcNow().ToUnixTimestamp();
	if (Second != LastSampleSecond)
	{
		LastSampleSecond = Second;
		EmitHostSample(World, Second);
	}
}

bool UWeaponSwarmNetHarnessSubsystem::SpawnGruntsNear(UWorld& World, const FVector& Origin, const float MinDistance,
	const float MaxDistance, int32& OutSpawned, FVector& OutCenter) const
{
	ISwarmBackend* Backend = USwarmSubsystem::Get(&World)->GetBackend();
	OutSpawned = 0;
	OutCenter = Origin;
	// 8 个方向 × 2 个半径依次尝试：有地面且生成数达标即采用，否则清掉重试
	constexpr int32 DirectionCount = 8;
	for (const float Distance : {MinDistance, MaxDistance})
	{
		for (int32 Direction = 0; Direction < DirectionCount; ++Direction)
		{
			const float Angle = 2.f * UE_PI * Direction / DirectionCount;
			const FVector Candidate = Origin + FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.f) * Distance;
			if (!SwarmHarnessHasGround(World, Candidate))
			{
				continue;
			}
			const int32 Spawned = Backend->SpawnAt(WeaponSwarmNetHarness::GruntRow, Candidate, WeaponSwarmNetHarness::SpawnCount);
			bool bFarEnough = true;
			if (MinDistance >= WeaponSwarmNetHarness::RecycleRequiredDistance)
			{
				Backend->ForEachAlive([&bFarEnough, &Origin](const FSwarmBugView& View)
				{
					bFarEnough &= FVector::Dist2D(View.Location, Origin) >= WeaponSwarmNetHarness::RecycleRequiredDistance;
				});
			}
			if (Spawned >= WeaponSwarmNetHarness::MinAcceptedSpawn && bFarEnough)
			{
				OutSpawned = Spawned;
				OutCenter = Candidate;
				return true;
			}
			Backend->DespawnAll();
		}
	}
	return false;
}

void UWeaponSwarmNetHarnessSubsystem::MakePlayersInvulnerable(UWorld& World) const
{
	const AGameStateBase* GameState = World.GetGameState();
	if (!GameState)
	{
		return;
	}
	for (APlayerState* PlayerState : GameState->PlayerArray)
	{
		const AWeaponPlayerState* WeaponPlayerState = Cast<AWeaponPlayerState>(PlayerState);
		UAbilitySystemComponent* ASC = WeaponPlayerState ? WeaponPlayerState->GetAbilitySystemComponent() : nullptr;
		if (!ASC || !ASC->HasAttributeSetForAttribute(UWeaponHealthSet::GetHealthAttribute()))
		{
			continue;
		}
		// 先抬 MaxHealth 再抬 Health（Health 被钳制到 MaxHealth）；受伤后补满
		if (ASC->GetNumericAttributeBase(UWeaponHealthSet::GetMaxHealthAttribute()) < WeaponSwarmNetHarness::InvulnerableHealth)
		{
			ASC->SetNumericAttributeBase(UWeaponHealthSet::GetMaxHealthAttribute(), WeaponSwarmNetHarness::InvulnerableHealth);
		}
		if (ASC->GetNumericAttributeBase(UWeaponHealthSet::GetHealthAttribute()) < WeaponSwarmNetHarness::InvulnerableHealth * 0.5f)
		{
			ASC->SetNumericAttributeBase(UWeaponHealthSet::GetHealthAttribute(), WeaponSwarmNetHarness::InvulnerableHealth);
		}
	}
}

void UWeaponSwarmNetHarnessSubsystem::EmitHostSample(UWorld& World, const int64 Second)
{
	ISwarmBackend* Backend = USwarmSubsystem::Get(&World)->GetBackend();
	FVector Sum = FVector::ZeroVector;
	int32 Alive = 0;
	Backend->ForEachAlive([&Sum, &Alive](const FSwarmBugView& View)
	{
		Sum += View.Location;
		++Alive;
	});
	const FVector Mean = Alive > 0 ? Sum / Alive : FVector::ZeroVector;
	EmitMarker(TEXT("HostSample"), FString::Printf(
		TEXT("Sec=%lld|Alive=%d|MeanX=%.1f|MeanY=%.1f|MeanZ=%.1f|Players=%d"),
		Second, Alive, Mean.X, Mean.Y, Mean.Z, LastPlayerCount));
}

void UWeaponSwarmNetHarnessSubsystem::TickClient(UWorld& World)
{
	if (World.GetNetMode() != NM_Client)
	{
		return;
	}
	const APawn* LocalPawn = SwarmHarnessLocalPawn(*GetGameInstance(), World);
	if (!bClientInWorldEmitted)
	{
		if (!LocalPawn)
		{
			return;
		}
		bClientInWorldEmitted = true;
		EmitMarker(TEXT("ClientInWorld"), FString::Printf(TEXT("NetMode=%s|Map=%s"),
			SwarmHarnessLexNetMode(World.GetNetMode()), *World.GetOutermost()->GetName()));
	}

	// 逐帧滑行检测：激活中的虫连续 N 帧单帧位移 > 阈值即计一次（每段连续只计一次）。
	// 刚激活的首帧不计位移（ActivationSerial 变化时客户端吸附属预期行为）。
	for (TActorIterator<ASwarmBugPawn> It(&World); It; ++It)
	{
		ASwarmBugPawn* Bug = *It;
		FBugTrack& Track = BugTracks.FindOrAdd(Bug);
		const bool bActive = Bug->IsBugActive();
		const FVector Location = Bug->GetActorLocation();
		if (bActive && Track.bWasActive)
		{
			if (FVector::Dist2D(Location, Track.LastLocation) > WeaponSwarmNetHarness::SlideFrameDistance)
			{
				if (++Track.ConsecutiveFastFrames == WeaponSwarmNetHarness::SlideFrameThreshold)
				{
					++WindowSlides;
					++TotalSlides;
				}
			}
			else
			{
				Track.ConsecutiveFastFrames = 0;
			}
		}
		else
		{
			Track.ConsecutiveFastFrames = 0;
		}
		Track.bWasActive = bActive;
		Track.LastLocation = Location;
	}

	const int64 Second = FDateTime::UtcNow().ToUnixTimestamp();
	if (Second != LastSampleSecond)
	{
		LastSampleSecond = Second;
		EmitClientSample(World, Second, FPlatformTime::Seconds());
	}
}

void UWeaponSwarmNetHarnessSubsystem::EmitClientSample(UWorld& World, const int64 Second, const double NowSeconds)
{
	FVector Sum = FVector::ZeroVector;
	int32 Visible = 0;
	for (TActorIterator<ASwarmBugPawn> It(&World); It; ++It)
	{
		if (It->IsBugActive())
		{
			Sum += It->GetActorLocation();
			++Visible;
		}
	}
	const FVector Mean = Visible > 0 ? Sum / Visible : FVector::ZeroVector;

	// 入站带宽：ServerConnection 累计入站字节差分（不依赖 StatPeriod）
	double InKBps = 0.0;
	const UNetDriver* NetDriver = World.GetNetDriver();
	const UNetConnection* Connection = NetDriver ? NetDriver->ServerConnection.Get() : nullptr;
	if (Connection)
	{
		const int64 InTotalBytes = Connection->InTotalBytes;
		if (LastInTotalBytes >= 0 && NowSeconds > LastBytesSampleSeconds)
		{
			InKBps = (InTotalBytes - LastInTotalBytes) / 1024.0 / (NowSeconds - LastBytesSampleSeconds);
		}
		LastInTotalBytes = InTotalBytes;
		LastBytesSampleSeconds = NowSeconds;
	}

	EmitMarker(TEXT("ClientSample"), FString::Printf(
		TEXT("Sec=%lld|Visible=%d|MeanX=%.1f|MeanY=%.1f|MeanZ=%.1f|InKBps=%.2f|Slides=%d|SlidesTotal=%d"),
		Second, Visible, Mean.X, Mean.Y, Mean.Z, InKBps, WindowSlides, TotalSlides));
	WindowSlides = 0;
}

void UWeaponSwarmNetHarnessSubsystem::HandleHostComplete(const bool bWasSuccessful, const FString Message)
{
	EmitMarker(bWasSuccessful ? TEXT("HostAdvertised") : TEXT("HostFailed"),
		FString::Printf(TEXT("Message=%s"), *SanitizeMarkerValue(Message)));
}

void UWeaponSwarmNetHarnessSubsystem::HandleFindComplete(const bool bWasSuccessful,
	const TArray<FWeaponLanSessionResult>& Results)
{
	bFindInFlight = false;
	if (ProcessRole != EProcessRole::Client || bJoinStarted)
	{
		return;
	}
	const FWeaponLanSessionResult* MatchingResult = Results.FindByPredicate(
		[this](const FWeaponLanSessionResult& Result) { return Result.ServerName == ServerName; });
	if (!bWasSuccessful || !MatchingResult)
	{
		NextFindAttemptSeconds = FPlatformTime::Seconds() + WeaponSwarmNetHarness::FindRetrySeconds;
		EmitMarker(TEXT("FindCompleted"), FString::Printf(TEXT("Success=%d|Count=%d|Match=0"),
			bWasSuccessful, Results.Num()));
		return;
	}

	UWeaponSessionSubsystem* SessionSubsystem = GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	bJoinStarted = SessionSubsystem && SessionSubsystem->JoinLanGame(MatchingResult->ResultIndex);
	EmitMarker(bJoinStarted ? TEXT("JoinRequested") : TEXT("JoinRequestRejected"),
		FString::Printf(TEXT("Result=%d|Ping=%d"), MatchingResult->ResultIndex, MatchingResult->PingInMs));
	if (!bJoinStarted)
	{
		NextFindAttemptSeconds = FPlatformTime::Seconds() + WeaponSwarmNetHarness::FindRetrySeconds;
	}
}

void UWeaponSwarmNetHarnessSubsystem::HandleJoinComplete(const bool bWasSuccessful, const FString Message)
{
	EmitMarker(bWasSuccessful ? TEXT("JoinTravelStarted") : TEXT("JoinFailed"),
		FString::Printf(TEXT("Message=%s"), *SanitizeMarkerValue(Message)));
	if (!bWasSuccessful)
	{
		bJoinStarted = false;
		NextFindAttemptSeconds = FPlatformTime::Seconds() + WeaponSwarmNetHarness::FindRetrySeconds;
	}
}

void UWeaponSwarmNetHarnessSubsystem::EmitMarker(const TCHAR* Event, const FString& Details) const
{
	// T = UTC Unix 毫秒，脚本用它做跨进程时间窗口计算
	UE_LOG(LogWeaponSwarmHarness, Display, TEXT("WSH|Run=%s|Process=%s|Event=%s|T=%lld%s%s"),
		*RunId, *ProcessLabel, Event, SwarmHarnessUnixMillis(),
		Details.IsEmpty() ? TEXT("") : TEXT("|"), *Details);
}

FString UWeaponSwarmNetHarnessSubsystem::SanitizeMarkerValue(FString Value)
{
	Value.ReplaceInline(TEXT("|"), TEXT("/"));
	Value.ReplaceInline(TEXT("\r"), TEXT(" "));
	Value.ReplaceInline(TEXT("\n"), TEXT(" "));
	return Value;
}
