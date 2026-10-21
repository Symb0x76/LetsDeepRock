#include "Swarm/SwarmSubsystem.h"

#include "Engine/World.h"
#include "HAL/IConsoleManager.h"

static TAutoConsoleVariable<int32> CVarSwarmBackend(
	TEXT("Swarm.Backend"), 0,
	TEXT("虫群后端：0=Pooled（池化 Actor），1=Mass（尚未实现，回退 Pooled）。"), ECVF_Default);

static TAutoConsoleVariable<int32> CVarSwarmPoolSize(
	TEXT("Swarm.PoolSize"), SwarmConstants::PoolCapacity,
	TEXT("虫群池容量，钳制到 [1, 4000]；后端初始化时读取一次，基准测试也使用。"), ECVF_Default);

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
	return World && (World->IsGameWorld());
}

void USwarmSubsystem::OnWorldBeginPlay(UWorld& InWorld)
{
	Super::OnWorldBeginPlay(InWorld);

	if (InWorld.GetNetMode() == NM_Client)
	{
		return;
	}

	// 目前只有 Pooled 一种实现；Task 6 在 Pooled 分支创建后端
	const ESwarmBackendKind Kind = ResolveBackendKind(CVarSwarmBackend.GetValueOnGameThread());
	if (Kind == ESwarmBackendKind::Pooled)
	{
		UE_LOG(LogTemp, Log, TEXT("[Swarm] Pooled backend not built yet"));
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
