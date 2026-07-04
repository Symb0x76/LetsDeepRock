// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponBase.h"
#include "WeaponDataAsset.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"

AWeaponBase::AWeaponBase()
{
	PrimaryActorTick.bCanEverTick = false;
	bReplicates = false; // 武器 Actor 作为角色的附属，不需要独立复制

	// 场景根
	SceneRoot = CreateDefaultSubobject<USceneComponent>(TEXT("SceneRoot"));
	RootComponent = SceneRoot;

	// 武器网格
	MeshComponent = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("WeaponMesh"));
	MeshComponent->SetupAttachment(SceneRoot);
	MeshComponent->SetCollisionProfileName(TEXT("NoCollision"));

	// 枪口挂点（默认位于网格前方，可在蓝图中调整偏移）
	MuzzlePoint = CreateDefaultSubobject<USceneComponent>(TEXT("MuzzlePoint"));
	MuzzlePoint->SetupAttachment(MeshComponent);
	MuzzlePoint->SetRelativeLocation(FVector(100.0f, 0.0f, 0.0f));
}

void AWeaponBase::InitFromDataAsset(const UWeaponDataAsset* InData)
{
	WeaponData = InData;
	if (!InData)
	{
		return;
	}

	// 加载并设置武器网格
	if (!InData->WeaponMesh.IsNull())
	{
		UStaticMesh* LoadedMesh = InData->WeaponMesh.LoadSynchronous();
		if (LoadedMesh && MeshComponent)
		{
			MeshComponent->SetStaticMesh(LoadedMesh);
		}
	}
}

FVector AWeaponBase::GetMuzzleLocation() const
{
	return MuzzlePoint ? MuzzlePoint->GetComponentLocation() : GetActorLocation();
}

FRotator AWeaponBase::GetMuzzleRotation() const
{
	return MuzzlePoint ? MuzzlePoint->GetComponentRotation() : GetActorRotation();
}
