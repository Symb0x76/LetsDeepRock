#include "WeaponStableIdentityComponent.h"

#include "Engine/World.h"
#include "Net/UnrealNetwork.h"
#include "WeaponSnapshotRegistry.h"

UWeaponStableIdentityComponent::UWeaponStableIdentityComponent()
{
	PrimaryComponentTick.bCanEverTick = false;
	SetIsReplicatedByDefault(true);
}

void UWeaponStableIdentityComponent::BeginPlay()
{
	Super::BeginPlay();

	if (!GetOwner() || GetOwner()->GetNetMode() == NM_Client)
	{
		return;
	}

	if (!AllocateIfNeeded() || !RegisterWithServerRegistry())
	{
		UE_LOG(LogTemp, Error, TEXT("[Snapshot] 无法为 %s 注册 Stable ID；对象不会进入可恢复注册表。"), *GetNameSafe(GetOwner()));
	}
}

void UWeaponStableIdentityComponent::EndPlay(const EEndPlayReason::Type EndPlayReason)
{
	UnregisterFromServerRegistry();
	Super::EndPlay(EndPlayReason);
}

bool UWeaponStableIdentityComponent::InitializeIdentity(
	const EWeaponStableEntityKind InKind,
	const FName InNamespace,
	const FString& InValue)
{
	if (!GetOwner() || GetOwner()->GetNetMode() == NM_Client || StableEntityId.IsValid())
	{
		return false;
	}

	if (InKind == EWeaponStableEntityKind::None || InNamespace == NAME_None || InValue.IsEmpty())
	{
		return false;
	}

	IdentityKind = InKind;
	IdentityNamespace = InNamespace;
	StableEntityId.Kind = InKind;
	StableEntityId.Namespace = InNamespace;
	StableEntityId.Value = InValue;
	return true;
}

bool UWeaponStableIdentityComponent::InitializeRuntimeIdentity(
	const EWeaponStableEntityKind InKind,
	const FName InNamespace)
{
	if (!GetOwner() || GetOwner()->GetNetMode() == NM_Client || StableEntityId.IsValid()
		|| InKind == EWeaponStableEntityKind::None || InNamespace == NAME_None)
	{
		return false;
	}

	IdentityKind = InKind;
	IdentityNamespace = InNamespace;
	bAutoAllocateRuntimeGuid = true;
	return AllocateIfNeeded();
}

bool UWeaponStableIdentityComponent::AllocateIfNeeded()
{
	if (StableEntityId.IsValid())
	{
		return true;
	}

	if (IdentityKind == EWeaponStableEntityKind::None || IdentityNamespace == NAME_None || !bAutoAllocateRuntimeGuid)
	{
		return false;
	}

	const FGuid RuntimeGuid = FGuid::NewGuid();
	if (!RuntimeGuid.IsValid())
	{
		return false;
	}

	StableEntityId.Kind = IdentityKind;
	StableEntityId.Namespace = IdentityNamespace;
	StableEntityId.Value = RuntimeGuid.ToString(EGuidFormats::DigitsWithHyphens);
	return StableEntityId.IsValid();
}

bool UWeaponStableIdentityComponent::RegisterWithServerRegistry()
{
	if (!GetOwner() || !StableEntityId.IsValid())
	{
		return false;
	}

	UWeaponSnapshotRegistry* Registry = GetWorld()->GetSubsystem<UWeaponSnapshotRegistry>();
	return Registry && Registry->RegisterActor(StableEntityId, GetOwner());
}

void UWeaponStableIdentityComponent::UnregisterFromServerRegistry()
{
	if (!GetWorld() || !GetOwner() || GetOwner()->GetNetMode() == NM_Client)
	{
		return;
	}

	if (UWeaponSnapshotRegistry* Registry = GetWorld()->GetSubsystem<UWeaponSnapshotRegistry>())
	{
		Registry->UnregisterActor(GetOwner());
	}
}

void UWeaponStableIdentityComponent::GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const
{
	Super::GetLifetimeReplicatedProps(OutLifetimeProps);
	DOREPLIFETIME(UWeaponStableIdentityComponent, StableEntityId);
}
