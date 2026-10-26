#include "Swarm/SwarmSubsystem.h"

#include "Engine/Engine.h"
#include "Engine/World.h"
#include "EngineUtils.h"
#include "GameFramework/Pawn.h"
#include "GridMap/GridMapManager.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "Swarm/PooledSwarmBackend.h"

static TAutoConsoleVariable<int32> CVarSwarmBackend(
	TEXT("Swarm.Backend"), 0,
	TEXT("虫群后端：0=Pooled（池化 Actor），1=Mass（尚未实现，回退 Pooled）。"), ECVF_Default);

static TAutoConsoleVariable<int32> CVarSwarmPoolSize(
	TEXT("Swarm.PoolSize"), SwarmConstants::PoolCapacity,
	TEXT("虫群池容量，钳制到 [1, 4000]；后端初始化时读取一次，基准测试也使用。"), ECVF_Default);

namespace SwarmSpawnCommandConstants
{
	inline constexpr float SpawnForwardOffset = 800.f;
}

/** Swarm.Spawn <Row> <Count>：在首个本地玩家前方 800uu 生成；无本地玩家时在世界原点生成（基准测试用） */
static void SwarmHandleSpawnCommand(const TArray<FString>& Args, UWorld* World)
{
	if (!World)
	{
		return;
	}
	if (World->GetNetMode() == NM_Client)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Swarm] server only"));
		return;
	}
	if (Args.Num() < 2)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Swarm] Usage: Swarm.Spawn <Row> <Count>"));
		return;
	}

	USwarmSubsystem* Subsystem = World->GetSubsystem<USwarmSubsystem>();
	ISwarmBackend* Backend = Subsystem ? Subsystem->GetBackend() : nullptr;
	if (!Backend)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Swarm] Swarm.Spawn: no backend in this world"));
		return;
	}

	const FName Row(*Args[0]);
	const int32 Count = FMath::Max(0, FCString::Atoi(*Args[1]));

	FVector Location = FVector::ZeroVector;
	const APlayerController* LocalController = GEngine ? GEngine->GetFirstLocalPlayerController(World) : nullptr;
	if (const APawn* LocalPawn = LocalController ? LocalController->GetPawn() : nullptr)
	{
		Location = LocalPawn->GetActorLocation() + LocalPawn->GetActorForwardVector() * SwarmSpawnCommandConstants::SpawnForwardOffset;
	}
	else
	{
		UE_LOG(LogTemp, Log, TEXT("[Swarm] Swarm.Spawn: no local player pawn, spawning at world origin"));
	}

	const int32 Spawned = Backend->SpawnAt(Row, Location, Count);
	UE_LOG(LogTemp, Log, TEXT("[Swarm] Swarm.Spawn %s: spawned %d/%d at %s (alive %d)"),
		*Row.ToString(), Spawned, Count, *Location.ToCompactString(), Backend->CountAlive());
}

static FAutoConsoleCommandWithWorldAndArgs SwarmSpawnCommand(
	TEXT("Swarm.Spawn"),
	TEXT("Swarm.Spawn <Row> <Count>：仅服务器，在首个本地玩家前方 800uu 生成虫群（无玩家时在原点）。"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SwarmHandleSpawnCommand));

/** 取服务器世界的后端；客户端/无后端时记日志并返回 nullptr */
static ISwarmBackend* SwarmGetServerBackend(UWorld* World)
{
	if (!World)
	{
		return nullptr;
	}
	if (World->GetNetMode() == NM_Client)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Swarm] server only"));
		return nullptr;
	}
	USwarmSubsystem* Subsystem = World->GetSubsystem<USwarmSubsystem>();
	ISwarmBackend* Backend = Subsystem ? Subsystem->GetBackend() : nullptr;
	if (!Backend)
	{
		UE_LOG(LogTemp, Warning, TEXT("[Swarm] no backend in this world"));
	}
	return Backend;
}

static void SwarmHandleStatsCommand(const TArray<FString>& Args, UWorld* World)
{
	if (const ISwarmBackend* Backend = SwarmGetServerBackend(World))
	{
		UE_LOG(LogTemp, Log, TEXT("[Swarm] backend=%s alive=%d pool=%d"),
			Backend->GetKind() == ESwarmBackendKind::Pooled ? TEXT("Pooled") : TEXT("Mass"),
			Backend->CountAlive(), Backend->GetCapacity());
	}
}

static void SwarmHandleKillAllCommand(const TArray<FString>& Args, UWorld* World)
{
	if (ISwarmBackend* Backend = SwarmGetServerBackend(World))
	{
		Backend->DespawnAll();
		UE_LOG(LogTemp, Log, TEXT("[Swarm] KillAll: alive=%d"), Backend->CountAlive());
	}
}

static FAutoConsoleCommandWithWorldAndArgs SwarmStatsCommand(
	TEXT("Swarm.Stats"),
	TEXT("Swarm.Stats：仅服务器，输出后端种类、存活数与池容量。"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SwarmHandleStatsCommand));

static FAutoConsoleCommandWithWorldAndArgs SwarmKillAllCommand(
	TEXT("Swarm.KillAll"),
	TEXT("Swarm.KillAll：仅服务器，回收全部存活虫群。"),
	FConsoleCommandWithWorldAndArgsDelegate::CreateStatic(&SwarmHandleKillAllCommand));

USwarmSubsystem* USwarmSubsystem::Get(const UObject* WorldContext)
{
	const UWorld* World = GEngine ? GEngine->GetWorldFromContextObject(WorldContext, EGetWorldErrorMode::ReturnNull) : nullptr;
	return World ? World->GetSubsystem<USwarmSubsystem>() : nullptr;
}

ESwarmBackendKind USwarmSubsystem::ResolveBackendKind(int32 CVarValue)
{
	switch (CVarValue)
	{
	case 0:
		return ESwarmBackendKind::Pooled;
	case 1:
		UE_LOG(LogTemp, Warning, TEXT("[Swarm] Mass backend unavailable, using Pooled"));
		return ESwarmBackendKind::Pooled;
	default:
		UE_LOG(LogTemp, Warning, TEXT("[Swarm] Unknown Swarm.Backend %d, using Pooled"), CVarValue);
		return ESwarmBackendKind::Pooled;
	}
}

int32 USwarmSubsystem::GetConfiguredPoolSize()
{
	return FMath::Clamp(CVarSwarmPoolSize.GetValueOnGameThread(), 1, 4000);
}

bool USwarmSubsystem::ShouldCreateSubsystem(UObject* Outer) const
{
	if (!Super::ShouldCreateSubsystem(Outer))
	{
		return false;
	}
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void USwarmSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (InWorld.GetNetMode() == NM_Client)
	{
		return;
	}

	// 只有放置了 GridMapManager 的玩法关卡才需要虫群；主菜单/大厅不建池。
	if (!TActorIterator<AGridMapManager>(&InWorld))
	{
		return;
	}

	// 目前只有 Pooled 一种实现
	const ESwarmBackendKind Kind = ResolveBackendKind(CVarSwarmBackend.GetValueOnGameThread());
	if (Kind == ESwarmBackendKind::Pooled)
	{
		Backend = MakeUnique<FPooledSwarmBackend>();
		Backend->Initialize(InWorld);
	}
}

void USwarmSubsystem::Deinitialize()
{
	if (Backend)
	{
		Backend->Deinitialize();
		Backend.Reset();
	}
	Super::Deinitialize();
}
