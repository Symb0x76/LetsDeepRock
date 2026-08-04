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
	UWorld* World = nullptr;

#if WITH_EDITOR
	// 编辑器控制台输入:只接受运行中的 PIE 世界;编辑器主界面无游戏世界,给出提示而不是静默失败。
	if (GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			if (Context.WorldType == EWorldType::PIE && Context.World() != nullptr)
			{
				World = Context.World();
				break;
			}
		}
	}
	if (!World)
	{
		UE_LOG(LogTemp, Warning, TEXT("[NetTestUI] 没有运行中的 Play 会话:请先打开地图并 Play,再输入 WeaponTestUI"));
		return;
	}
#else
	World = GWorld;
#endif

	if (!World)
	{
		UE_LOG(LogTemp, Warning, TEXT("[NetTestUI] 无可用世界,命令被忽略"));
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
		UE_LOG(LogTemp, Error, TEXT("[NetTestUI] Widget 创建失败"));
		return;
	}

	Widget->AddToViewport(100);
	Widget->SetAlignmentInViewport(FVector2D(1.0f, 0.0f));
	Widget->SetPositionInViewport(FVector2D(-12.0f, 12.0f));
	GNetTestWidget = Widget;
	UE_LOG(LogTemp, Log, TEXT("[NetTestUI] 面板已创建(再输入 WeaponTestUI 或按 F2 关闭)"));
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
	UWeaponSessionSubsystem* Session = GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	SessionOpText->SetText(FText::Format(
		FText::FromString(TEXT("操作: {0}")),
		FText::FromString(LexSessionOperation(Session->GetCurrentOperation()))));
}

void UWeaponNetTestWidget::RefreshMatchState()
{
	AWeaponGameState* GameState = GetWorld()->GetGameState<AWeaponGameState>();
	if (!GameState)
	{
		MatchStateText->SetText(FText::FromString(TEXT("阶段: -")));
		return;
	}

	FString ReadyInfo = TEXT("-");
	FString SlotInfo = TEXT("-");
	if (APlayerController* PC = GetOwningPlayer())
	{
		if (AWeaponPlayerState* PS = PC->GetPlayerState<AWeaponPlayerState>())
		{
			ReadyInfo = PS->GetIsPlayerReady() ? TEXT("是") : TEXT("否");
			SlotInfo = FString::Printf(TEXT("%d"), PS->GetPlayerSlotIndex());
		}
	}

	MatchStateText->SetText(FText::Format(
		FText::FromString(TEXT("阶段: {0} 剩余: {1}s 玩家: {2}/{3} | Slot: {4} Ready: {5} | 恢复屏障: {6}")),
		FText::FromString(LexMatchPhase(GameState->GetMatchPhase())),
		FText::AsNumber(GameState->GetRemainingPhaseTime()),
		FText::AsNumber(GameState->GetConnectedPlayerCount()),
		FText::AsNumber(GameState->GetMaxPlayers()),
		FText::FromString(SlotInfo),
		FText::FromString(ReadyInfo),
		FText::FromString(GameState->IsGameplayRecoveryBlocked() ? TEXT("开") : TEXT("关"))));
}

void UWeaponNetTestWidget::RefreshMigrationState()
{
	UWeaponHostMigrationSubsystem* Migration = GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>();

	EWeaponMigrationRole LocalRole = EWeaponMigrationRole::None;
	if (APlayerController* PC = GetOwningPlayer())
	{
		if (APlayerState* PS = PC->PlayerState)
		{
			LocalRole = Migration->GetRoleForPlayer(PS->GetUniqueId());
		}
	}

	MigrationStateText->SetText(FText::Format(
		FText::FromString(TEXT("状态: {0} | 本机角色: {1}")),
		FText::FromString(LexMigrationState(Migration->GetMigrationState())),
		FText::FromString(LexMigrationRole(LocalRole))));

	FString AssignmentLine = TEXT("角色分配: -");
	if (const FWeaponMigrationRoleAssignment* Assignment = Migration->GetCurrentRoleAssignment())
	{
		AssignmentLine = FString::Printf(TEXT("Epoch:%llu RosterRev:%llu | 主机:%s(S%d) A:%s(S%d) B:%s(S%d)"),
			(unsigned long long)Assignment->AuthorityEpoch,
			(unsigned long long)Assignment->RosterRevision,
			*Assignment->ActiveHost.PlayerId.ToDebugString(), Assignment->ActiveHost.OriginalSlot,
			*Assignment->BackupA.PlayerId.ToDebugString(), Assignment->BackupA.OriginalSlot,
			*Assignment->BackupB.PlayerId.ToDebugString(), Assignment->BackupB.OriginalSlot);
	}
	RolesText->SetText(FText::FromString(AssignmentLine));

	const int64 CommonRevision = Migration->GetCommonCommittedRevision();
	const int64 LocalSaveRevision = Migration->GetLastLocalSaveRevision();
	FWeaponMigrationPayloadAck CommittedAck;
	const bool bHasCommitted = Migration->GetCommittedAck(CommittedAck);
	const FString CommittedStr = bHasCommitted
		? FString::Printf(TEXT("rev %lld"), (long long)CommittedAck.SnapshotRevision)
		: TEXT("无");

	SnapshotText->SetText(FText::Format(
		FText::FromString(TEXT("快照: 共同提交 rev {0} | 本机提交 {1} | 本地存档 rev {2}")),
		FText::AsNumber(CommonRevision),
		FText::FromString(CommittedStr),
		FText::AsNumber(LocalSaveRevision)));

	int32 ReceivedChunks = 0;
	int32 TotalChunks = 0;
	Migration->GetPendingPayloadProgress(ReceivedChunks, TotalChunks);
	const FString PendingStr = (TotalChunks > 0)
		? FString::Printf(TEXT("%d/%d 块"), ReceivedChunks, TotalChunks)
		: TEXT("无");

	PendingText->SetText(FText::Format(
		FText::FromString(TEXT("Pending: {0} | 完整: {1} | Committed: {2}")),
		FText::FromString(PendingStr),
		FText::FromString(Migration->IsPendingPayloadComplete() ? TEXT("是") : TEXT("否")),
		FText::FromString(Migration->HasCommittedPayload() ? TEXT("是") : TEXT("否"))));

	OverlayText->SetText(FText::Format(
		FText::FromString(TEXT("覆盖层: {0} 进度: {1} | {2}")),
		FText::FromString(Migration->IsMigrationOverlayVisible() ? TEXT("显示") : TEXT("隐藏")),
		FText::AsNumber(Migration->GetMigrationProgress()),
		Migration->GetMigrationOverlayMessage()));
}

void UWeaponNetTestWidget::RefreshResultList()
{
	UWeaponSessionSubsystem* Session = GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	const TArray<FWeaponLanSessionResult>& Results = Session->GetCachedLanResults();
	if (Results.Num() == CachedResultCount)
	{
		return;
	}
	CachedResultCount = Results.Num();

	SessionResultBox->ClearChildren();
	for (int32 Index = 0; Index < Results.Num() && Index < 10; ++Index)
	{
		const FWeaponLanSessionResult& Result = Results[Index];
		UHorizontalBox* Row = NewObject<UHorizontalBox>(this);
		SessionResultBox->AddChild(Row);

		AddText(Row, FString::Printf(TEXT("[%d] %s %d/%d %dms"),
			Index, *Result.ServerName, Result.CurrentPlayers, Result.MaxPlayers, Result.PingInMs));

		UWeaponNetTestJoinButton* JoinButton = NewObject<UWeaponNetTestJoinButton>(this);
		UTextBlock* LabelText = NewObject<UTextBlock>(this);
		LabelText->SetText(FText::FromString(TEXT("Join")));
		LabelText->SetFont(FAppStyle::GetFontStyle(TEXT("SmallFont")));
		JoinButton->AddChild(LabelText);
		JoinButton->ResultIndex = Index;
		JoinButton->OnClicked.AddDynamic(JoinButton, &UWeaponNetTestJoinButton::HandleSelfClicked);
		JoinButton->OnJoinIndexClicked.AddDynamic(this, &UWeaponNetTestWidget::HandleJoinResultClicked);
		Row->AddChild(JoinButton);
	}
}

void UWeaponNetTestJoinButton::HandleSelfClicked()
{
	OnJoinIndexClicked.Broadcast(ResultIndex);
}

void UWeaponNetTestWidget::UpdateButtonEnablement()
{
	UWeaponSessionSubsystem* Session = GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	UWeaponHostMigrationSubsystem* Migration = GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>();

	const bool bSessionIdle = Session->GetCurrentOperation() == EWeaponSessionOperation::Idle;
	const bool bHasAuthority = GetWorld()->GetAuthGameMode() != nullptr;

	EWeaponMigrationRole LocalRole = EWeaponMigrationRole::None;
	if (APlayerController* PC = GetOwningPlayer())
	{
		if (APlayerState* PS = PC->PlayerState)
		{
			LocalRole = Migration->GetRoleForPlayer(PS->GetUniqueId());
		}
	}

	const EWeaponHostMigrationState MigrationState = Migration->GetMigrationState();
	const bool bIsActiveHost = LocalRole == EWeaponMigrationRole::ActiveHost;
	const bool bCanHandoff = bIsActiveHost
		&& (MigrationState == EWeaponHostMigrationState::Monitoring || MigrationState == EWeaponHostMigrationState::Idle);

	HostButton->SetIsEnabled(bSessionIdle);
	FindButton->SetIsEnabled(bSessionIdle);
	DirectJoinButton->SetIsEnabled(bSessionIdle && !AddressBox->GetText().IsEmpty());
	ReconnectButton->SetIsEnabled(bSessionIdle && !AddressBox->GetText().IsEmpty());
	DestroyButton->SetIsEnabled(bSessionIdle);
	ReadyButton->SetIsEnabled(true);
	StartMatchButton->SetIsEnabled(bHasAuthority);
	DisconnectButton->SetIsEnabled(true);
	PlannedHandoffButton->SetIsEnabled(bCanHandoff);
	RestartLocalSaveButton->SetIsEnabled(bIsActiveHost);
	CaptureNowButton->SetIsEnabled(bIsActiveHost);
}

void UWeaponNetTestWidget::HandleHostClicked()
{
	UWeaponSessionSubsystem* Session = GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	FString ServerName = ServerNameBox->GetText().ToString();
	if (ServerName.IsEmpty())
	{
		ServerName = TEXT("TestHost");
	}
	const int32 MaxPlayers = FMath::Clamp(FCString::Atoi(*MaxPlayersBox->GetText().ToString()), 1, 4);
	Session->HostLanGame(MaxPlayers, ServerName);
}

void UWeaponNetTestWidget::HandleFindClicked()
{
	UWeaponSessionSubsystem* Session = GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	Session->FindLanGames(50);
}

void UWeaponNetTestWidget::HandleDirectJoinClicked()
{
	UWeaponSessionSubsystem* Session = GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	Session->JoinDirectAddress(AddressBox->GetText().ToString());
}

void UWeaponNetTestWidget::HandleReconnectMigratedClicked()
{
	UWeaponSessionSubsystem* Session = GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	Session->ReconnectToMigratedHost(AddressBox->GetText().ToString());
}

void UWeaponNetTestWidget::HandleDestroyClicked()
{
	UWeaponSessionSubsystem* Session = GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	Session->DestroySession();
}

void UWeaponNetTestWidget::HandleJoinResultClicked(int32 ResultIndex)
{
	UWeaponSessionSubsystem* Session = GetGameInstance()->GetSubsystem<UWeaponSessionSubsystem>();
	Session->JoinLanGame(ResultIndex);
}

void UWeaponNetTestWidget::HandleReadyClicked()
{
	if (APlayerController* PC = GetOwningPlayer())
	{
		if (AWeaponPlayerState* PS = PC->GetPlayerState<AWeaponPlayerState>())
		{
			PS->RequestSetPlayerReady(!PS->GetIsPlayerReady());
		}
	}
}

void UWeaponNetTestWidget::HandleStartMatchClicked()
{
	if (AWeaponGameMode* GameMode = Cast<AWeaponGameMode>(GetWorld()->GetAuthGameMode()))
	{
		GameMode->TryStartWeaponMatch();
	}
}

void UWeaponNetTestWidget::HandleDisconnectClicked()
{
	GEngine->Exec(GetWorld(), TEXT("Disconnect"));
}

void UWeaponNetTestWidget::HandlePlannedHandoffClicked()
{
	UWeaponHostMigrationSubsystem* Migration = GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>();
	Migration->RequestPlannedHostHandoff();
}

void UWeaponNetTestWidget::HandleRestartLocalSaveClicked()
{
	UWeaponHostMigrationSubsystem* Migration = GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>();
	Migration->RestartLatestLocalSaveAsNewMatch(ServerNameBox->GetText().ToString());
}

void UWeaponNetTestWidget::HandleCaptureNowClicked()
{
	UWeaponHostMigrationSubsystem* Migration = GetGameInstance()->GetSubsystem<UWeaponHostMigrationSubsystem>();
	FString Error;
	const bool bOk = Migration->CaptureAndDistributeSnapshot(Error);
	UE_LOG(LogTemp, Warning, TEXT("[NetTestUI] CaptureAndDistributeSnapshot: %s %s"),
		bOk ? TEXT("OK") : TEXT("FAIL"), *Error);
}

#endif // !UE_BUILD_SHIPPING
