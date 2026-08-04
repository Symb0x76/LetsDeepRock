// Copyright Epic Games, Inc. All Rights Reserved.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "WeaponNetTestWidget.generated.h"

class UButton;
class UEditableTextBox;
class UHorizontalBox;
class UPanelWidget;
class UTextBlock;
class UVerticalBox;

/**
 * 网络测试调试面板(仅非 Shipping 构建)。
 *
 * 控制台命令 WeaponTestUI 切换开/关,F2 同效。
 * 直接 C++ 访问 WeaponSessionSubsystem / WeaponHostMigrationSubsystem /
 * GameState / PlayerState,不经过蓝图层;NativeTick 每 0.25 秒刷新监控文本与按钮可用性。
 */
UCLASS()
class UWeaponNetTestWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** 控制台命令入口;面板已存在则关闭,否则创建。 */
	static void ToggleViaConsole();

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	// ---- 布局构建 ----
	void BuildLayout();
	UButton* AddButton(UPanelWidget* Parent, const FString& Label);
	UTextBlock* AddText(UPanelWidget* Parent, const FString& Text, const FLinearColor& Color = FLinearColor::White);
	void AddSectionHeader(UPanelWidget* Parent, const FString& Title);

	// ---- 状态刷新 ----
	void RefreshAll();
	void RefreshSessionState();
	void RefreshMatchState();
	void RefreshMigrationState();
	void RefreshResultList();
	void UpdateButtonEnablement();
	void ToggleSelf();

	// ---- Session 操作 ----
	UFUNCTION()
	void HandleHostClicked();
	UFUNCTION()
	void HandleFindClicked();
	UFUNCTION()
	void HandleDirectJoinClicked();
	UFUNCTION()
	void HandleReconnectMigratedClicked();
	UFUNCTION()
	void HandleDestroyClicked();
	UFUNCTION()
	void HandleJoinResultClicked(int32 ResultIndex);

	// ---- Match 操作 ----
	UFUNCTION()
	void HandleReadyClicked();
	UFUNCTION()
	void HandleStartMatchClicked();

	// ---- 破坏性操作 ----
	UFUNCTION()
	void HandleDisconnectClicked();
	UFUNCTION()
	void HandlePlannedHandoffClicked();
	UFUNCTION()
	void HandleRestartLocalSaveClicked();
	UFUNCTION()
	void HandleCaptureNowClicked();

	// ---- 控件引用 ----
	UPROPERTY()
	TObjectPtr<UVerticalBox> RootBox;
	UPROPERTY()
	TObjectPtr<UVerticalBox> SessionResultBox;
	UPROPERTY()
	TObjectPtr<UButton> HostButton;
	UPROPERTY()
	TObjectPtr<UButton> FindButton;
	UPROPERTY()
	TObjectPtr<UButton> DirectJoinButton;
	UPROPERTY()
	TObjectPtr<UButton> ReconnectButton;
	UPROPERTY()
	TObjectPtr<UButton> DestroyButton;
	UPROPERTY()
	TObjectPtr<UButton> ReadyButton;
	UPROPERTY()
	TObjectPtr<UButton> StartMatchButton;
	UPROPERTY()
	TObjectPtr<UButton> DisconnectButton;
	UPROPERTY()
	TObjectPtr<UButton> PlannedHandoffButton;
	UPROPERTY()
	TObjectPtr<UButton> RestartLocalSaveButton;
	UPROPERTY()
	TObjectPtr<UButton> CaptureNowButton;
	UPROPERTY()
	TObjectPtr<UEditableTextBox> ServerNameBox;
	UPROPERTY()
	TObjectPtr<UEditableTextBox> MaxPlayersBox;
	UPROPERTY()
	TObjectPtr<UEditableTextBox> AddressBox;
	UPROPERTY()
	TObjectPtr<UTextBlock> SessionOpText;
	UPROPERTY()
	TObjectPtr<UTextBlock> MatchStateText;
	UPROPERTY()
	TObjectPtr<UTextBlock> MigrationStateText;
	UPROPERTY()
	TObjectPtr<UTextBlock> RolesText;
	UPROPERTY()
	TObjectPtr<UTextBlock> SnapshotText;
	UPROPERTY()
	TObjectPtr<UTextBlock> PendingText;
	UPROPERTY()
	TObjectPtr<UTextBlock> OverlayText;

	float RefreshTimer = 0.0f;
	int32 CachedResultCount = -1;
};
