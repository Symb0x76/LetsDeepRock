#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "WeaponMatchSnapshot.h"
#include "WeaponSnapshotRegistry.generated.h"

/** 服务器当前 World 中 Stable ID 与 Actor 的双向索引。 */
UCLASS()
class LETSDEEPROCK_API UWeaponSnapshotRegistry : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;

	/** 仅服务器注册；重复 ID 或重复 Actor 都会失败。 */
	bool RegisterActor(const FWeaponStableEntityId& Id, AActor* Actor);
	void UnregisterActor(AActor* Actor);
	void UnregisterId(const FWeaponStableEntityId& Id);

	AActor* FindActor(const FWeaponStableEntityId& Id) const;
	bool FindId(const AActor* Actor, FWeaponStableEntityId& OutId) const;
	bool Contains(const FWeaponStableEntityId& Id) const;
	void Reset();

private:
	UPROPERTY()
	TMap<FWeaponStableEntityId, TWeakObjectPtr<AActor>> ActorsById;

	TMap<TWeakObjectPtr<AActor>, FWeaponStableEntityId> IdsByActor;
};
