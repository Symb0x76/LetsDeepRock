#pragma once

#include "CoreMinimal.h"
#include "Components/ActorComponent.h"
#include "WeaponMatchSnapshot.h"
#include "WeaponStableIdentityComponent.generated.h"

/**
 * 可复用的服务器权威实体身份组件。
 *
 * 组件只负责身份生命周期：分配、复制、注册和注销；具体 Snapshot 状态仍由 Coordinator 捕获。
 */
UCLASS(ClassGroup = (Snapshot), BlueprintType, Blueprintable, meta = (BlueprintSpawnableComponent))
class LETSDEEPROCK_API UWeaponStableIdentityComponent : public UActorComponent
{
	GENERATED_BODY()

public:
	UWeaponStableIdentityComponent();

	virtual void BeginPlay() override;
	virtual void EndPlay(const EEndPlayReason::Type EndPlayReason) override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	/** 在 BeginPlay 前由服务器为 Player 等外部身份写入固定值。 */
	UFUNCTION(BlueprintCallable, Category = "Snapshot|Identity")
	bool InitializeIdentity(EWeaponStableEntityKind InKind, FName InNamespace, const FString& InValue);

	/** 服务器 Spawn 后使用 RuntimeGuid 生成身份；适合没有外部账号 ID 的 NPC/投射物。 */
	UFUNCTION(BlueprintCallable, Category = "Snapshot|Identity")
	bool InitializeRuntimeIdentity(EWeaponStableEntityKind InKind, FName InNamespace = TEXT("RuntimeGuid"));

	UFUNCTION(BlueprintPure, Category = "Snapshot|Identity")
	const FWeaponStableEntityId& GetStableEntityId() const { return StableEntityId; }

	UFUNCTION(BlueprintPure, Category = "Snapshot|Identity")
	bool HasValidIdentity() const { return StableEntityId.IsValid(); }

protected:
	/** 蓝图默认值；服务器 BeginPlay 时若 Value 为空则生成 RuntimeGuid。 */
	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Snapshot|Identity")
	EWeaponStableEntityKind IdentityKind = EWeaponStableEntityKind::None;

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Snapshot|Identity")
	FName IdentityNamespace = TEXT("RuntimeGuid");

	UPROPERTY(EditDefaultsOnly, BlueprintReadOnly, Category = "Snapshot|Identity")
	bool bAutoAllocateRuntimeGuid = true;

	UPROPERTY(Replicated, VisibleInstanceOnly, BlueprintReadOnly, Category = "Snapshot|Identity")
	FWeaponStableEntityId StableEntityId;

private:
	bool AllocateIfNeeded();
	bool RegisterWithServerRegistry();
	void UnregisterFromServerRegistry();
};
