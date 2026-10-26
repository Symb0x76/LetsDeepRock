// Swarm 池化虫群：世界子系统，按 Swarm.Backend 选择并持有后端

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Swarm/SwarmBackend.h"
#include "SwarmSubsystem.generated.h"

/** 虫群世界子系统：仅 Game/PIE 世界创建；后端仅在非客户端的 OnWorldBeginPlay 创建 */
UCLASS()
class LETSDEEPROCK_API USwarmSubsystem : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	static USwarmSubsystem* Get(const UObject* WorldContext);

	/** 将 Swarm.Backend 的值解析为后端种类；未实现或未知值回退到 Pooled 并告警 */
	static ESwarmBackendKind ResolveBackendKind(int32 CVarValue);

	/** Swarm.PoolSize 钳制到 [1, 4000] 后的值，后端初始化与基准测试共用 */
	static int32 GetConfiguredPoolSize();

	/** 客户端以及 OnWorldBeginPlay 之前为 nullptr */
	ISwarmBackend* GetBackend() const { return Backend.Get(); }

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;

protected:
	virtual void OnWorldBeginPlay(UWorld& InWorld) override;

private:
	TUniquePtr<ISwarmBackend> Backend;
};
