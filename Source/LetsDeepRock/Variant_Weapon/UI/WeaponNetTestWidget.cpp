// Copyright Epic Games, Inc. All Rights Reserved.

#if !UE_BUILD_SHIPPING

#include "Variant_Weapon/UI/WeaponNetTestWidget.h"

#include "Blueprint/WidgetTree.h"
#include "Components/Button.h"
#include "Components/EditableTextBox.h"
#include "Components/HorizontalBox.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Engine/Engine.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "HAL/IConsoleManager.h"
#include "InputCoreTypes.h"
#include "Online/WeaponHostMigrationSubsystem.h"
#include "Online/WeaponSessionSubsystem.h"
#include "Styling/AppStyle.h"
#include "Variant_Weapon/WeaponGameMode.h"
#include "Variant_Weapon/WeaponGameState.h"
#include "Variant_Weapon/WeaponPlayerState.h"

namespace
{
	static TWeakObjectPtr<UWeaponNetTestWidget> GNetTestWidget;

	static const TCHAR* LexMigrationState(EWeaponHostMigrationState State)
	{
		switch (State)
		{
		case EWeaponHostMigrationState::Idle: return TEXT("Idle");
		case EWeaponHostMigrationState::Monitoring: return TEXT("Monitoring");
		case EWeaponHostMigrationState::WaitingForDisconnectTravel: return TEXT("WaitingForDisconnectTravel");
		case EWeaponHostMigrationState::PreparingCandidate: return TEXT("PreparingCandidate");
		case EWeaponHostMigrationState::SearchingCandidate: return TEXT("SearchingCandidate");
		case EWeaponHostMigrationState::JoiningCandidate: return TEXT("JoiningCandidate");
		case EWeaponHostMigrationState::Restoring: return TEXT("Restoring");
		case EWeaponHostMigrationState::AdvertisingCandidate: return TEXT("AdvertisingCandidate");
		case EWeaponHostMigrationState::WaitingForReadyAcks: return TEXT("WaitingForReadyAcks");
		case EWeaponHostMigrationState::PlannedHandoff: return TEXT("PlannedHandoff");
		case EWeaponHostMigrationState::LoadingLocalFork: return TEXT("LoadingLocalFork");
		case EWeaponHostMigrationState::PreparingLocalFork: return TEXT("PreparingLocalFork");
		case EWeaponHostMigrationState::RestoringLocalFork: return TEXT("RestoringLocalFork");
		case EWeaponHostMigrationState::AdvertisingLocalFork: return TEXT("AdvertisingLocalFork");
		case EWeaponHostMigrationState::Failed: return TEXT("Failed");
		default: return TEXT("Unknown");
		}
	}

	static const TCHAR* LexMigrationRole(EWeaponMigrationRole Role)
	{
		switch (Role)
		{
		case EWeaponMigrationRole::None: return TEXT("None");
		case EWeaponMigrationRole::ActiveHost: return TEXT("ActiveHost");
		case EWeaponMigrationRole::BackupA: return TEXT("BackupA");
		case EWeaponMigrationRole::BackupB: return TEXT("BackupB");
		default: return TEXT("Unknown");
		}
	}

	static const TCHAR* LexSessionOperation(EWeaponSessionOperation Op)
	{
		switch (Op)
		{
		case EWeaponSessionOperation::Idle: return TEXT("Idle");
		case EWeaponSessionOperation::HostingTravel: return TEXT("HostingTravel");
		case EWeaponSessionOperation::Creating: return TEXT("Creating");
		case EWeaponSessionOperation::Finding: return TEXT("Finding");
		case EWeaponSessionOperation::Joining: return TEXT("Joining");
		case EWeaponSessionOperation::Destroying: return TEXT("Destroying");
		case EWeaponSessionOperation::DirectTravel: return TEXT("DirectTravel");
		default: return TEXT("Unknown");
		}
	}

	static const TCHAR* LexMatchPhase(EWeaponMatchPhase Phase)
	{
		switch (Phase)
		{
		case EWeaponMatchPhase::Waiting: return TEXT("Waiting");
		case EWeaponMatchPhase::Starting: return TEXT("Starting");
		case EWeaponMatchPhase::InGame: return TEXT("InGame");
		case EWeaponMatchPhase::Ending: return TEXT("Ending");
		default: return TEXT("Unknown");
		}
	}

	static FAutoConsoleCommand CmdWeaponNetTestUI(
		TEXT("WeaponTestUI"),
		TEXT("Toggle the network test UI overlay (non-shipping builds only)."),
		FConsoleCommandDelegate::CreateStatic(&UWeaponNetTestWidget::ToggleViaConsole));
}

void UWeaponNetTestWidget::ToggleViaConsole()
{
	UWorld* World = GWorld;
	if (!World)
	{
		return;
	}

	if (GNetTestWidget.IsValid())
	{
		GNetTestWidget->ToggleSelf();
		return;
	}

	UWeaponNetTestWidget* Widget = CreateWidget<UWeaponNetTestWidget>(World, UWeaponNetTestWidget::StaticClass());
	if (!Widget)
	{
		return;
	}

	Widget->AddToViewport(100);
	Widget->SetAlignmentInViewport(FVector2D(1.0f, 0.0f));
	Widget->SetPositionInViewport(FVector2D(-12.0f, 12.0f));
	GNetTestWidget = Widget;
}

void UWeaponNetTestWidget::ToggleSelf()
{
	RemoveFromParent();
	GNetTestWidget.Reset();
}

void UWeaponNetTestWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();
	BuildLayout();
	RefreshAll();
}

void UWeaponNetTestWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (APlayerController* PC = GetOwningPlayer())
	{
		if (PC->WasInputKeyJustPressed(EKeys::F2))
		{
			ToggleSelf();
			return;
		}
	}

	RefreshTimer += InDeltaTime;
	if (RefreshTimer >= 0.25f)
	{
		RefreshTimer = 0.0f;
		RefreshAll();
	}
}

void UWeaponNetTestWidget::BuildLayout()
{
	USizeBox* SizeBox = NewObject<USizeBox>(this);
	SizeBox->SetWidthOverride(620.0f);

	RootBox = NewObject<UVerticalBox>(this);
	SizeBox->AddChild(RootBox);
	WidgetTree->RootWidget = SizeBox;

	AddText(RootBox, TEXT("Net Test (F2)"), FLinearColor(1.0f, 0.85f, 0.3f));
	SessionOpText = AddText(RootBox, TEXT("操作: Idle"));

	// ---- Session ----
	AddSectionHeader(RootBox, TEXT("Session"));

	UHorizontalBox* HostRow = NewObject<UHorizontalBox>(this);
	RootBox->AddChild(HostRow);

	ServerNameBox = NewObject<UEditableTextBox>(this);
	ServerNameBox->SetText(FText::FromString(TEXT("TestHost")));
	ServerNameBox->SetMinDesiredWidth(170.0f);
	HostRow->AddChild(ServerNameBox);

	MaxPlayersBox = NewObject<UEditableTextBox>(this);
	MaxPlayersBox->SetText(FText::FromString(TEXT("4")));
	MaxPlayersBox->SetMinDesiredWidth(42.0f);
	HostRow->AddChild(MaxPlayersBox);

	HostButton = AddButton(HostRow, TEXT("Host"));
	FindButton = AddButton(HostRow, TEXT("Find"));

	SessionResultBox = NewObject<UVerticalBox>(this);
	RootBox->AddChild(SessionResultBox);

	UHorizontalBox* AddressRow = NewObject<UHorizontalBox>(this);
	RootBox->AddChild(AddressRow);

	AddressBox = NewObject<UEditableTextBox>(this);
	AddressBox->SetText(FText::FromString(TEXT("127.0.0.1:7777")));
	AddressBox->SetMinDesiredWidth(260.0f);
	AddressRow->AddChild(AddressBox);

	DirectJoinButton = AddButton(AddressRow, TEXT("直连 Join"));
	ReconnectButton = AddButton(AddressRow, TEXT("重连迁移"));

	DestroyButton = AddButton(RootBox, TEXT("Destroy Session"));

	// ---- Match ----
	AddSectionHeader(RootBox, TEXT("Match"));
	MatchStateText = AddText(RootBox, TEXT("阶段: -"));

	UHorizontalBox* MatchRow = NewObject<UHorizontalBox>(this);
	RootBox->AddChild(MatchRow);

	ReadyButton = AddButton(MatchRow, TEXT("切换 Ready"));
	StartMatchButton = AddButton(MatchRow, TEXT("开始战局"));

	// ---- Migration ----
	AddSectionHeader(RootBox, TEXT("Migration"));
	MigrationStateText = AddText(RootBox, TEXT("状态: -"));
	RolesText = AddText(RootBox, TEXT("角色: -"));
	SnapshotText = AddText(RootBox, TEXT("快照: -"));
	PendingText = AddText(RootBox, TEXT("Pending: -"));
	OverlayText = AddText(RootBox, TEXT("覆盖层: -"));

	// ---- 破坏性操作 ----
	AddSectionHeader(RootBox, TEXT("破坏性操作"));

	UHorizontalBox* DestructiveRowA = NewObject<UHorizontalBox>(this);
	RootBox->AddChild(DestructiveRowA);

	DisconnectButton = AddButton(DestructiveRowA, TEXT("断开本端"));
	PlannedHandoffButton = AddButton(DestructiveRowA, TEXT("计划交接"));

	UHorizontalBox* DestructiveRowB = NewObject<UHorizontalBox>(this);
	RootBox->AddChild(DestructiveRowB);

	RestartLocalSaveButton = AddButton(DestructiveRowB, TEXT("本地存档新战局"));
	CaptureNowButton = AddButton(DestructiveRowB, TEXT("立即快照"));

	HostButton->OnClicked.AddDynamic(this, &UWeaponNetTestWidget::HandleHostClicked);
	FindButton->OnClicked.AddDynamic(this, &UWeaponNetTestWidget::HandleFindClicked);
	DirectJoinButton->OnClicked.AddDynamic(this, &UWeaponNetTestWidget::HandleDirectJoinClicked);
	ReconnectButton->OnClicked.AddDynamic(this, &UWeaponNetTestWidget::HandleReconnectMigratedClicked);
	DestroyButton->OnClicked.AddDynamic(this, &UWeaponNetTestWidget::HandleDestroyClicked);
	ReadyButton->OnClicked.AddDynamic(this, &UWeaponNetTestWidget::HandleReadyClicked);
	StartMatchButton->OnClicked.AddDynamic(this, &UWeaponNetTestWidget::HandleStartMatchClicked);
	DisconnectButton->OnClicked.AddDynamic(this, &UWeaponNetTestWidget::HandleDisconnectClicked);
	PlannedHandoffButton->OnClicked.AddDynamic(this, &UWeaponNetTestWidget::HandlePlannedHandoffClicked);
	RestartLocalSaveButton->OnClicked.AddDynamic(this, &UWeaponNetTestWidget::HandleRestartLocalSaveClicked);
	CaptureNowButton->OnClicked.AddDynamic(this, &UWeaponNetTestWidget::HandleCaptureNowClicked);
}

UButton* UWeaponNetTestWidget::AddButton(UPanelWidget* Parent, const FString& Label)
{
	UButton* Button = NewObject<UButton>(this);
	UTextBlock* LabelText = NewObject<UTextBlock>(this);
	LabelText->SetText(FText::FromString(Label));
	LabelText->SetFont(FAppStyle::GetFontStyle(TEXT("SmallFont")));
	Button->AddChild(LabelText);
	Parent->AddChild(Button);
	return Button;
}

UTextBlock* UWeaponNetTestWidget::AddText(UPanelWidget* Parent, const FString& Text, const FLinearColor& Color)
{
	UTextBlock* Block = NewObject<UTextBlock>(this);
	Block->SetText(FText::FromString(Text));
	Block->SetColorAndOpacity(FSlateColor(Color));
	Block->SetFont(FAppStyle::GetFontStyle(TEXT("SmallFont")));
	Parent->AddChild(Block);
	return Block;
}

void UWeaponNetTestWidget::AddSectionHeader(UPanelWidget* Parent, const FString& Title)
{
	UTextBlock* Header = NewObject<UTextBlock>(this);
	Header->SetText(FText::FromString(Title));
	Header->SetFont(FAppStyle::GetFontStyle(TEXT("BoldFont")));
	Header->SetColorAndOpacity(FSlateColor(FLinearColor(0.55f, 0.8f, 1.0f)));
	Parent->AddChild(Header);
}

void UWeaponNetTestWidget::RefreshAll()
{
	RefreshSessionState();
	RefreshMatchState();
	RefreshMigrationState();
	RefreshResultList();
	UpdateButtonEnablement();
}

void UWeaponNetTestWidget::RefreshSessionState()
{
	// Task 3 填充
}

void UWeaponNetTestWidget::RefreshMatchState()
{
	// Task 3 填充
}

void UWeaponNetTestWidget::RefreshMigrationState()
{
	// Task 4 填充
}

void UWeaponNetTestWidget::RefreshResultList()
{
	// Task 3 填充
}

void UWeaponNetTestWidget::UpdateButtonEnablement()
{
	// Task 4 填充(含 BOSS 贡献点)
}

void UWeaponNetTestWidget::HandleHostClicked() {}
void UWeaponNetTestWidget::HandleFindClicked() {}
void UWeaponNetTestWidget::HandleDirectJoinClicked() {}
void UWeaponNetTestWidget::HandleReconnectMigratedClicked() {}
void UWeaponNetTestWidget::HandleDestroyClicked() {}
void UWeaponNetTestWidget::HandleJoinResultClicked(int32 ResultIndex) {}
void UWeaponNetTestWidget::HandleReadyClicked() {}
void UWeaponNetTestWidget::HandleStartMatchClicked() {}
void UWeaponNetTestWidget::HandleDisconnectClicked() {}
void UWeaponNetTestWidget::HandlePlannedHandoffClicked() {}
void UWeaponNetTestWidget::HandleRestartLocalSaveClicked() {}
void UWeaponNetTestWidget::HandleCaptureNowClicked() {}

#endif // !UE_BUILD_SHIPPING
