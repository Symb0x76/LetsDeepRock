// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/PlayerState.h"
#include "AbilitySystemInterface.h"
#include "WeaponPlayerState.generated.h"

class UAbilitySystemComponent;
class UWeaponHealthSet;
class UWeaponCombatSet;
class UWeaponMovementSet;
class UWeaponResistanceSet;
class UWeaponResourceSet;

USTRUCT(BlueprintType)
struct FWeaponPlayerLobbyState
{
	GENERATED_BODY()
	
	UPROPERTY(BlueprintReadOnly, Category = "Player")
	int32 SlotIndex = INDEX_NONE;
	
	UPROPERTY(BlueprintReadOnly, Category = "Player")
	bool bIsReady = false;
	
	// 重载 == 和 !=
	bool operator==(const FWeaponPlayerLobbyState& Other) const
	{
		return SlotIndex == Other.SlotIndex
			&& bIsReady == Other.bIsReady;
	}

	bool operator!=(const FWeaponPlayerLobbyState& Other) const
	{
		return !(*this == Other);
	}
};

// 动态多播委托
DECLARE_DYNAMIC_MULTICAST_DELEGATE_TwoParams(
	FOnWeaponPlayerLobbyStateChanged,
	FWeaponPlayerLobbyState, OldState,
	FWeaponPlayerLobbyState, NewState);

/**
 *  武器系统玩家状态
 *
 *  按 GAS 标准范式，玩家的 AbilitySystemComponent 挂在 PlayerState 上：
 *  - OwnerActor = 本 PlayerState
 *  - AvatarActor = 当前被 Possess 的 WeaponCharacter
 *  好处：属性/效果在 Pawn 重生间持久化，天然适配联机。
 *
 *  ASC 的 InitAbilityActorInfo 由 WeaponCharacter 在
 *  PossessedBy（服务端）/ OnRep_PlayerState（客户端）中调用。
 */
UCLASS()
class AWeaponPlayerState : public APlayerState, public IAbilitySystemInterface
{
	GENERATED_BODY()

public:

	AWeaponPlayerState();

	//~ IAbilitySystemInterface
	virtual UAbilitySystemComponent* GetAbilitySystemComponent() const override;
	virtual void GetLifetimeReplicatedProps(TArray<FLifetimeProperty>& OutLifetimeProps) const override;

	UFUNCTION(BlueprintPure, Category = "Vision")
	int32 GetTeamId() const { return TeamId; }

	UFUNCTION(BlueprintCallable, Category = "Vision")
	void SetTeamId(int32 InTeamId);

	/** 类型化访问 HealthSet */
	UWeaponHealthSet* GetHealthSet() const { return HealthSet; }

	/** 类型化访问 CombatSet */
	UWeaponCombatSet* GetCombatSet() const { return CombatSet; }

	/** 类型化访问 MovementSet */
	UWeaponMovementSet* GetMovementSet() const { return MovementSet; }

	/** 类型化访问 ResistanceSet */
	UWeaponResistanceSet* GetResistanceSet() const { return ResistanceSet; }

	/** 类型化访问 ResourceSet */
	UWeaponResourceSet* GetResourceSet() const { return ResourceSet; }

	UFUNCTION(BlueprintPure, Category = "Player")
	int32 GetPlayerSlotIndex() const { return LobbyState.SlotIndex; }
	
	UFUNCTION(BlueprintPure, Category = "Player")
	bool GetIsPlayerReady() const { return LobbyState.bIsReady; }

	const FWeaponPlayerLobbyState& GetLobbyState() const
	{
		return LobbyState;
	}

	/** 仅服务端调用；由 WeaponGameMode 在 PostLogin 时分配 */
	void SetPlayerSlotIndex(int32 NewSlotIndex);

	/** 仅服务端调用；用于写入已验证的 ready 状态。 */
	void SetPlayerReady(bool bNewIsReady);

	/** 本地玩家提交 ready 意图；远程客户端通过 Server RPC，Listen Host 直接执行。 */
	UFUNCTION(BlueprintCallable, Category = "Player")
	void RequestSetPlayerReady(bool bNewIsReady);

	UPROPERTY(BlueprintAssignable, Category = "Player")
	FOnWeaponPlayerLobbyStateChanged OnLobbyStateChanged;
	
protected:
	virtual void OnDeactivated() override;

	UFUNCTION(Server, Reliable)
	void ServerSetPlayerReady(bool bNewIsReady);

	void ApplyPlayerReadyRequest(bool bNewIsReady);

	UPROPERTY(Replicated, EditAnywhere, BlueprintReadOnly, Category = "Vision")
	int32 TeamId = 0;

	/** 能力系统组件（在 PlayerState 上创建并复制） */
	UPROPERTY(VisibleAnywhere, Category = "GAS")
	TObjectPtr<UAbilitySystemComponent> AbilitySystemComponent;

	/** 生命值属性集 */
	UPROPERTY()
	TObjectPtr<UWeaponHealthSet> HealthSet;

	/** 战斗属性集 */
	UPROPERTY()
	TObjectPtr<UWeaponCombatSet> CombatSet;

	/** 移动属性集 */
	UPROPERTY()
	TObjectPtr<UWeaponMovementSet> MovementSet;

	/** 抗性属性集 */
	UPROPERTY()
	TObjectPtr<UWeaponResistanceSet> ResistanceSet;

	/** 资源属性集 */
	UPROPERTY()
	TObjectPtr<UWeaponResourceSet> ResourceSet;
	
	UPROPERTY(ReplicatedUsing = OnRep_LobbyState, BlueprintReadOnly, Category = "Player")
	FWeaponPlayerLobbyState LobbyState;
	
	UFUNCTION()
	void OnRep_LobbyState(FWeaponPlayerLobbyState OldState);
};
