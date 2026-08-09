// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Interfaces/OnlineSessionInterface.h"
#include "OnlineSessionSettings.h"
#include "Online/WeaponHostMigrationTypes.h"
#include "WeaponGameState.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "WeaponSessionSubsystem.generated.h"

USTRUCT(BlueprintType)
struct FWeaponLanSessionResult
{
	GENERATED_BODY()

	/** 传给 JoinLanGame 的稳定索引，仅对最近一次搜索有效 */
	UPROPERTY(BlueprintReadOnly, Category = "LAN Session")
	int32 ResultIndex = INDEX_NONE;

	UPROPERTY(BlueprintReadOnly, Category = "LAN Session")
	FString ServerName;

	UPROPERTY(BlueprintReadOnly, Category = "LAN Session")
	int32 PingInMs = 0;

	UPROPERTY(BlueprintReadOnly, Category = "LAN Session")
	int32 CurrentPlayers = 0;

	UPROPERTY(BlueprintReadOnly, Category = "LAN Session")
	int32 MaxPlayers = 0;

	UPROPERTY(BlueprintReadOnly, Category = "LAN Session")
	FString MapName;
};

UENUM(BlueprintType)
enum class EWeaponSessionOperation : uint8
{
	Idle,
	HostingTravel,
	Creating,
	Finding,
	Joining,
	Destroying,
	DirectTravel,
};

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FOnWeaponSessionOperationComplete,
	bool, bWasSuccessful,
	FString, Message);

DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FOnWeaponFindLanSessionsComplete,
	bool, bWasSuccessful,
	const TArray<FWeaponLanSessionResult>&, Results);

DECLARE_MULTICAST_DELEGATE_TwoParams(
	FOnWeaponRecoverySessionOperationComplete,
	bool,
	const FString&);

DECLARE_MULTICAST_DELEGATE_TwoParams(
	FOnWeaponLanHostOperationComplete,
	bool,
	const FString&);

/**
 * LAN/P2P Session 控制面
 *
 * 所有 OnlineSubsystem delegate 都在此，UI 只调用公开方法并监听 Blueprint delegate
 */

UCLASS()
class UWeaponSessionSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;
	virtual void Deinitialize() override;

	/** 在当前地图以 ?listen 重载，NetDriver 就绪后创建并广播 LAN Session。 */
	UFUNCTION(BlueprintCallable, Category = "LAN Session")
	bool HostLanGame(int32 MaxPlayers, const FString& ServerName);

	/** 前往指定地图并以 ?listen 启动，NetDriver 就绪后创建并广播 LAN Session。 */
	UFUNCTION(BlueprintCallable, Category = "LAN Session")
	bool HostLanGameOnMap(int32 MaxPlayers, const FString& ServerName, FName MapPackageName);

	/** 搜索同一局域网内由 OnlineSubsystemNull 广播的 Session。 */
	UFUNCTION(BlueprintCallable, Category = "LAN Session")
	bool FindLanGames(int32 MaxResults = 50);

	/** 加入最近一次 FindLanGames 返回的指定结果。 */
	UFUNCTION(BlueprintCallable, Category = "LAN Session")
	bool JoinLanGame(int32 ResultIndex);

	/** 绕过 Session 搜索，直接 ClientTravel 到 IPv4/IPv6/hostname 地址。 */
	UFUNCTION(BlueprintCallable, Category = "LAN Session")
	bool JoinDirectAddress(const FString& Address);

	/** 销毁本机名为 GameSession 的 Session。 */
	UFUNCTION(BlueprintCallable, Category = "LAN Session")
	bool DestroySession();

	/** 销毁旧主机 Session 记录后，直连迁移选举得到的新主机地址。 */
	UFUNCTION(BlueprintCallable, Category = "LAN Session")
	bool ReconnectToMigratedHost(const FString& Address);

	/** Destroy the old client Session, travel to a fresh gated listen World, then report preparation. */
	bool PrepareRecoveryHost(const FWeaponRecoverySessionDescriptor& Descriptor);

	/** Advertise a successfully restored candidate World. */
	bool AdvertiseRecoverySession(const FWeaponRecoverySessionDescriptor& Descriptor);

	/** Find and join only a Ready Session whose complete recovery identity matches Expected. */
	bool FindAndJoinRecoverySession(const FWeaponRecoverySessionDescriptor& Expected);

	/** Cancel recovery Session work and return to the same map as a standalone Waiting/Lobby World. */
	bool ReturnToRecoveryLobby(FName MapPackageName);

	const FWeaponRecoverySessionDescriptor* GetSelectedRecoverySession() const
	{
		return bHasSelectedRecoverySession ? &SelectedRecoverySession : nullptr;
	}

	FOnWeaponRecoverySessionOperationComplete OnRecoveryWorldPrepared;
	FOnWeaponRecoverySessionOperationComplete OnRecoveryHostComplete;
	FOnWeaponRecoverySessionOperationComplete OnRecoveryJoinComplete;
	FOnWeaponLanHostOperationComplete OnLanHostComplete;

	UFUNCTION(BlueprintPure, Category = "LAN Session")
	EWeaponSessionOperation GetCurrentOperation() const { return CurrentOperation; }

	UFUNCTION(BlueprintPure, Category = "LAN Session")
	TArray<FWeaponLanSessionResult> GetCachedLanResults() const { return CachedLanResults; }

	UPROPERTY(BlueprintAssignable, Category = "LAN Session")
	FOnWeaponSessionOperationComplete OnHostComplete;

	UPROPERTY(BlueprintAssignable, Category = "LAN Session")
	FOnWeaponFindLanSessionsComplete OnFindComplete;

	UPROPERTY(BlueprintAssignable, Category = "LAN Session")
	FOnWeaponSessionOperationComplete OnJoinComplete;

	UPROPERTY(BlueprintAssignable, Category = "LAN Session")
	FOnWeaponSessionOperationComplete OnDestroyComplete;

private:
	enum class EPendingAction : uint8
	{
		None,
		Host,
		JoinSearchResult,
		DirectTravel,
		RecoveryHost,
		RecoveryJoinSearchResult,
		RecoveryLobby,
	};

	IOnlineSessionPtr GetSessionInterface() const;
	FName ResolveCurrentMapName() const;

	bool BeginHostTravel();
	bool BeginRecoveryHostTravel();
	bool BeginCreateSession();
	bool BeginCreateSessionInternal(const FWeaponRecoverySessionDescriptor* RecoveryDescriptor);
	bool BeginJoinSession(int32 ResultIndex);
	bool BeginDestroySession();
	bool BeginDirectTravel(const FString& Address);
	bool BeginRecoveryLobbyTravel();
	bool BeginRecoverySearch();
	bool TryReadMatchingRecoverySession(
		const FOnlineSessionSearchResult& SearchResult,
		FWeaponRecoverySessionDescriptor& OutDescriptor) const;
	void ResetRecoveryOperationFlags();
	void ExecutePendingAction();
	void FailPendingAction(const FString& Message);
	void BroadcastHostComplete(bool bWasSuccessful, const FString& Message);

	void HandlePostLoadMap(UWorld* LoadedWorld);
	void HandleCreateSessionComplete(FName SessionName, bool bWasSuccessful);
	void HandleFindSessionsComplete(bool bWasSuccessful);
	void HandleJoinSessionComplete(FName SessionName, EOnJoinSessionCompleteResult::Type Result);
	void HandleDestroySessionComplete(FName SessionName, bool bWasSuccessful);

	void ClearOnlineDelegates();
	void SetOperation(EWeaponSessionOperation NewOperation) { CurrentOperation = NewOperation; }

	UPROPERTY(BlueprintReadOnly, Category = "LAN Session", meta = (AllowPrivateAccess = "true"))
	EWeaponSessionOperation CurrentOperation = EWeaponSessionOperation::Idle;

	UPROPERTY()
	TArray<FWeaponLanSessionResult> CachedLanResults;

	TSharedPtr<FOnlineSessionSearch> ActiveSearch;

	EPendingAction PendingAction = EPendingAction::None;
	int32 PendingHostMaxPlayers = 4;
	FString PendingHostServerName;
	FName PendingHostMapName = NAME_None;
	int32 PendingJoinResultIndex = INDEX_NONE;
	FString PendingDirectAddress;
	FName PendingRecoveryLobbyMapName = NAME_None;
	FWeaponRecoverySessionDescriptor PendingRecoverySession;
	FWeaponRecoverySessionDescriptor ExpectedRecoverySession;
	FWeaponRecoverySessionDescriptor SelectedRecoverySession;
	bool bCreatingRecoverySession = false;
	bool bFindingRecoverySession = false;
	bool bJoiningRecoverySession = false;
	bool bHasSelectedRecoverySession = false;

	FDelegateHandle CreateSessionDelegateHandle;
	FDelegateHandle FindSessionsDelegateHandle;
	FDelegateHandle JoinSessionDelegateHandle;
	FDelegateHandle DestroySessionDelegateHandle;
	FDelegateHandle PostLoadMapDelegateHandle;
};
