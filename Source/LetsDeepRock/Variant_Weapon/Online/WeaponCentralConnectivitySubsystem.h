// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "WeaponCentralConnectivityTypes.h"
#include "WeaponHostMigrationTypes.h"
#include "WeaponCentralConnectivitySubsystem.generated.h"

class UWeaponHostMigrationSubsystem;
class UWeaponSessionSubsystem;
class UWeaponSnapshotCoordinator;
struct FWeaponMigrationBackendReadiness;

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FOnWeaponCentralConnectivityChanged,
	EWeaponCentralConnectivityState, NewState,
	FString, Message);

/**
 * 中心控制面适配器:只把中心已验证的事实(Readiness / Lease / Endpoint)
 * 交给现有系统,不直接改 GameMode,不推进迁移状态机。
 *
 * 决策权始终在 UWeaponHostMigrationSubsystem;本子系统是外围传感器。
 */
UCLASS(Config = Game, DefaultConfig)
class LETSDEEPROCK_API UWeaponCentralConnectivitySubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	EWeaponCentralConnectivityState GetConnectivityState() const { return ConnectivityState; }

	/** 触发完整中心接入流程(设备身份 → 控制连接 → Lease → 候选 → readiness)。 */
	UFUNCTION(BlueprintCallable, Category = "Central Connectivity")
	void StartCentralConnect();

	UFUNCTION(BlueprintPure, Category = "Central Connectivity")
	bool IsCentralControlReady() const { return bCentralControlReady; }

	UFUNCTION(BlueprintPure, Category = "Central Connectivity")
	bool IsCentralLeaseReady() const { return bCentralLeaseReady; }

	UFUNCTION(BlueprintPure, Category = "Central Connectivity")
	bool IsCentralCheckpointReady() const { return bCentralCheckpointReady; }

	UPROPERTY(BlueprintAssignable, Category = "Central Connectivity")
	FOnWeaponCentralConnectivityChanged OnConnectivityChanged;

	/** Host: 上传一个候选到中心。 */
	bool UploadCandidate(const FWeaponCentralEndpointCandidate& Candidate, FString& OutError);

	/** Host: 用中心已确认的 direct endpoint 触发客户端 reconnect。 */
	bool TriggerDirectReconnect(const FString& Address, FString& OutError);

private:
	// —— 事实输入管线(第 2 课:HTTP 设备身份) ——
	void RunAuthPipeline();                 // challenge → register → session
	void SendChallengeRequest();            // POST /v1/devices/challenge
	void SendRegisterRequest(const FString& Nonce); // POST /v1/devices/register
	void SendSessionRequest(const FString& Nonce, const TArray<uint8>& Signature); // POST /v1/devices/session
	void HandleSessionSuccess(const FString& Token);
	void FailAuthPipeline(const FString& Message);
	void OpenEventChannel();                // WebSocket(第 3 课)
	void HandleLeaseGranted(const FWeaponCentralHostLease& Lease);
	void HandleHostEndpointNominated(const FWeaponCentralEndpointCandidate& Endpoint);
	void HandleCheckpointCommitted(const FGuid& MatchId, uint64 Epoch, int64 Revision);

	// —— 向现有系统交付事实(第 1 课) ——
	void PublishBackendReadiness();

	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity")
	FString CoordinatorBaseUrl = TEXT("https://coordinator.example.com");

	UPROPERTY(Config, EditDefaultsOnly, Category = "Central Connectivity")
	FString StunServerUrl = TEXT("stun:coordinator.example.com:3478");

	UPROPERTY()
	TArray<uint8> DevicePrivateKey;

	UPROPERTY()
	TArray<uint8> DevicePublicKey;

	UPROPERTY()
	FString PendingChallengeNonce;

	UPROPERTY()
	FString DeviceId;

	UPROPERTY()
	FString AccessToken;

	UPROPERTY()
	FGuid MatchId;

	UPROPERTY()
	uint64 AuthorityEpoch = 0;

	UPROPERTY()
	uint64 RosterRevision = 0;

	EWeaponCentralConnectivityState ConnectivityState = EWeaponCentralConnectivityState::Disconnected;
	EWeaponCentralAuthState AuthState = EWeaponCentralAuthState::None;
	bool bCentralControlReady = false;
	bool bCentralLeaseReady = false;
	bool bCentralCheckpointReady = false;

	TWeakObjectPtr<UWeaponHostMigrationSubsystem> HostMigration;
	TWeakObjectPtr<UWeaponSessionSubsystem> SessionSubsystem;
	TWeakObjectPtr<UWeaponSnapshotCoordinator> SnapshotCoordinator;
};
