// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponCombatHUDWidget.h"

#include "AbilitySystemComponent.h"
#include "Attributes/WeaponHealthSet.h"
#include "Attributes/WeaponResourceSet.h"
#include "Blueprint/WidgetTree.h"
#include "Components/Border.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/HorizontalBox.h"
#include "Components/HorizontalBoxSlot.h"
#include "Components/ProgressBar.h"
#include "Components/SizeBox.h"
#include "Components/TextBlock.h"
#include "Components/VerticalBox.h"
#include "Components/VerticalBoxSlot.h"
#include "Engine/World.h"
#include "GameFramework/Pawn.h"
#include "GameplayEffect.h"
#include "Styling/CoreStyle.h"
#include "Weapons/WeaponDataAsset.h"
#include "Weapons/WeaponEquipmentComponent.h"
#include "WeaponGameplayTags.h"
#include "WeaponGameState.h"
#include "WeaponPlayerState.h"

namespace CombatHUDStyle
{
	const FLinearColor PanelColor(0.02f, 0.025f, 0.03f, 0.72f);
	const FLinearColor LabelColor(0.62f, 0.66f, 0.70f, 1.0f);
	const FLinearColor ValueColor(0.92f, 0.94f, 0.96f, 1.0f);
	const FLinearColor HealthColor(0.85f, 0.18f, 0.16f, 1.0f);
	const FLinearColor ShieldColor(0.22f, 0.55f, 0.95f, 1.0f);
	const FLinearColor EnergyColor(0.95f, 0.72f, 0.12f, 1.0f);
	const FLinearColor WarningColor(1.0f, 0.55f, 0.15f, 1.0f);
	const FLinearColor ReadyColor(0.35f, 0.85f, 0.45f, 1.0f);
	constexpr float BarWidth = 240.0f;
	constexpr float BarHeight = 12.0f;
}

void UWeaponCombatHUDWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	if (!WidgetTree)
	{
		WidgetTree = NewObject<UWidgetTree>(this, TEXT("WidgetTree"));
	}

	UCanvasPanel* RootCanvas = Cast<UCanvasPanel>(GetRootWidget());
	if (!RootCanvas)
	{
		RootCanvas = WidgetTree->ConstructWidget<UCanvasPanel>();
		WidgetTree->RootWidget = RootCanvas;
	}
	HUDCanvas = RootCanvas;

	// 纯展示层:整棵树不参与命中测试,鼠标瞄准与点击穿透到游戏视口。
	SetVisibility(ESlateVisibility::HitTestInvisible);
	BuildLayout(RootCanvas);
	RefreshPhaseVisibility();
}

void UWeaponCombatHUDWidget::NativeTick(const FGeometry& MyGeometry, const float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	RefreshPhaseVisibility();
	TryBindAbilitySystem();
	if (!bHUDVisible)
	{
		return;
	}

	RefreshWeapon();
	RefreshGrenadeCooldown();
	RefreshRespawn();
}

void UWeaponCombatHUDWidget::NativeDestruct()
{
	UnbindAbilitySystem();
	Super::NativeDestruct();
}

void UWeaponCombatHUDWidget::BuildLayout(UCanvasPanel* RootCanvas)
{
	// ---- 左下:武器/弹药/手雷 + 三条属性条 ----
	UBorder* Panel = WidgetTree->ConstructWidget<UBorder>();
	Panel->SetBrushColor(CombatHUDStyle::PanelColor);
	Panel->SetPadding(FMargin(14.0f, 10.0f));

	UVerticalBox* Stack = WidgetTree->ConstructWidget<UVerticalBox>();
	Panel->SetContent(Stack);

	WeaponText = MakeText(TEXT("—"), 12, CombatHUDStyle::LabelColor, false);
	Stack->AddChildToVerticalBox(WeaponText);

	AmmoText = MakeText(TEXT("— / —"), 22, CombatHUDStyle::ValueColor, true);
	Stack->AddChildToVerticalBox(AmmoText)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 2.0f));

	GrenadeText = MakeText(TEXT("手雷 就绪"), 12, CombatHUDStyle::ReadyColor, false);
	Stack->AddChildToVerticalBox(GrenadeText)->SetPadding(FMargin(0.0f, 0.0f, 0.0f, 8.0f));

	HealthBar = AddVitalBar(Stack, TEXT("生命"), CombatHUDStyle::HealthColor);
	ShieldBar = AddVitalBar(Stack, TEXT("护盾"), CombatHUDStyle::ShieldColor);
	EnergyBar = AddVitalBar(Stack, TEXT("能量"), CombatHUDStyle::EnergyColor);

	UCanvasPanelSlot* PanelSlot = RootCanvas->AddChildToCanvas(Panel);
	PanelSlot->SetAnchors(FAnchors(0.0f, 1.0f));
	PanelSlot->SetAlignment(FVector2D(0.0f, 1.0f));
	PanelSlot->SetPosition(FVector2D(24.0f, -24.0f));
	PanelSlot->SetAutoSize(true);

	// ---- 屏幕中上:复活倒计时(仅等待复活时显示) ----
	RespawnText = MakeText(FString(), 28, CombatHUDStyle::WarningColor, true);
	RespawnText->SetJustification(ETextJustify::Center);
	RespawnText->SetVisibility(ESlateVisibility::Collapsed);

	UCanvasPanelSlot* RespawnSlot = RootCanvas->AddChildToCanvas(RespawnText);
	RespawnSlot->SetAnchors(FAnchors(0.5f, 0.35f));
	RespawnSlot->SetAlignment(FVector2D(0.5f, 0.5f));
	RespawnSlot->SetAutoSize(true);
}

UWeaponCombatHUDWidget::FVitalBar UWeaponCombatHUDWidget::AddVitalBar(
	UVerticalBox* Parent, const FString& Label, const FLinearColor& FillColor)
{
	UHorizontalBox* Row = WidgetTree->ConstructWidget<UHorizontalBox>();

	UTextBlock* LabelText = MakeText(Label, 11, CombatHUDStyle::LabelColor, false);
	UHorizontalBoxSlot* LabelSlot = Row->AddChildToHorizontalBox(LabelText);
	LabelSlot->SetPadding(FMargin(0.0f, 0.0f, 8.0f, 0.0f));
	LabelSlot->SetVerticalAlignment(VAlign_Center);

	UProgressBar* Bar = WidgetTree->ConstructWidget<UProgressBar>();
	Bar->SetFillColorAndOpacity(FillColor);
	Bar->SetPercent(1.0f);

	USizeBox* BarSize = WidgetTree->ConstructWidget<USizeBox>();
	BarSize->SetWidthOverride(CombatHUDStyle::BarWidth);
	BarSize->SetHeightOverride(CombatHUDStyle::BarHeight);
	BarSize->SetContent(Bar);
	UHorizontalBoxSlot* BarSlot = Row->AddChildToHorizontalBox(BarSize);
	BarSlot->SetVerticalAlignment(VAlign_Center);

	UTextBlock* ValueText = MakeText(TEXT("—"), 11, CombatHUDStyle::ValueColor, false);
	UHorizontalBoxSlot* ValueSlot = Row->AddChildToHorizontalBox(ValueText);
	ValueSlot->SetPadding(FMargin(8.0f, 0.0f, 0.0f, 0.0f));
	ValueSlot->SetVerticalAlignment(VAlign_Center);

	Parent->AddChildToVerticalBox(Row)->SetPadding(FMargin(0.0f, 2.0f));

	FVitalBar Result;
	Result.Bar = Bar;
	Result.Value = ValueText;
	return Result;
}

UTextBlock* UWeaponCombatHUDWidget::MakeText(
	const FString& InText, const int32 FontSize, const FLinearColor& Color, const bool bBold)
{
	UTextBlock* Text = WidgetTree->ConstructWidget<UTextBlock>();
	Text->SetText(FText::FromString(InText));
	Text->SetFont(FCoreStyle::GetDefaultFontStyle(bBold ? TEXT("Bold") : TEXT("Regular"), FontSize));
	Text->SetColorAndOpacity(FSlateColor(Color));
	return Text;
}

void UWeaponCombatHUDWidget::TryBindAbilitySystem()
{
	const AWeaponPlayerState* PS = GetOwningPlayerState<AWeaponPlayerState>();
	UAbilitySystemComponent* ASC = PS ? PS->GetAbilitySystemComponent() : nullptr;
	if (ASC == BoundASC.Get())
	{
		return;
	}

	UnbindAbilitySystem();
	if (!ASC)
	{
		return;
	}

	// 原生多播委托:AddUObject 绑定,Unbind 时 RemoveAll(this) 一次清掉。
	const FGameplayAttribute VitalAttributes[] =
	{
		UWeaponHealthSet::GetHealthAttribute(),
		UWeaponHealthSet::GetMaxHealthAttribute(),
		UWeaponHealthSet::GetShieldAttribute(),
		UWeaponHealthSet::GetMaxShieldAttribute(),
		UWeaponResourceSet::GetEnergyAttribute(),
		UWeaponResourceSet::GetMaxEnergyAttribute(),
	};
	for (const FGameplayAttribute& Attribute : VitalAttributes)
	{
		ASC->GetGameplayAttributeValueChangeDelegate(Attribute)
			.AddUObject(this, &UWeaponCombatHUDWidget::HandleVitalAttributeChanged);
	}
	BoundASC = ASC;

	// 绑定前已复制到的值不会再触发委托,先主动刷新一次。
	RefreshVitals();
}

void UWeaponCombatHUDWidget::UnbindAbilitySystem()
{
	UAbilitySystemComponent* ASC = BoundASC.Get();
	if (!ASC)
	{
		BoundASC.Reset();
		return;
	}

	const FGameplayAttribute VitalAttributes[] =
	{
		UWeaponHealthSet::GetHealthAttribute(),
		UWeaponHealthSet::GetMaxHealthAttribute(),
		UWeaponHealthSet::GetShieldAttribute(),
		UWeaponHealthSet::GetMaxShieldAttribute(),
		UWeaponResourceSet::GetEnergyAttribute(),
		UWeaponResourceSet::GetMaxEnergyAttribute(),
	};
	for (const FGameplayAttribute& Attribute : VitalAttributes)
	{
		ASC->GetGameplayAttributeValueChangeDelegate(Attribute).RemoveAll(this);
	}
	BoundASC.Reset();
}

void UWeaponCombatHUDWidget::HandleVitalAttributeChanged(const FOnAttributeChangeData& ChangeData)
{
	// 任一相关属性变化都整体重算三条:当前值与上限可能分别到达,统一读 AttributeSet 当前值最简单且不会错位。
	RefreshVitals();
}

void UWeaponCombatHUDWidget::RefreshVitals()
{
	const AWeaponPlayerState* PS = GetOwningPlayerState<AWeaponPlayerState>();
	const UWeaponHealthSet* HealthSet = PS ? PS->GetHealthSet() : nullptr;
	const UWeaponResourceSet* ResourceSet = PS ? PS->GetResourceSet() : nullptr;

	auto ApplyBar = [](const FVitalBar& Vital, const float Current, const float Max)
	{
		if (!Vital.Bar || !Vital.Value)
		{
			return;
		}
		Vital.Bar->SetPercent(Max > 0.0f ? FMath::Clamp(Current / Max, 0.0f, 1.0f) : 0.0f);
		Vital.Value->SetText(FText::FromString(
			FString::Printf(TEXT("%d / %d"), FMath::CeilToInt(Current), FMath::RoundToInt(Max))));
	};

	if (HealthSet)
	{
		ApplyBar(HealthBar, HealthSet->GetHealth(), HealthSet->GetMaxHealth());
		ApplyBar(ShieldBar, HealthSet->GetShield(), HealthSet->GetMaxShield());
	}
	if (ResourceSet)
	{
		ApplyBar(EnergyBar, ResourceSet->GetEnergy(), ResourceSet->GetMaxEnergy());
	}
}

void UWeaponCombatHUDWidget::RefreshWeapon()
{
	const APawn* Pawn = GetOwningPlayerPawn();
	const UWeaponEquipmentComponent* Equipment =
		Pawn ? Pawn->FindComponentByClass<UWeaponEquipmentComponent>() : nullptr;
	const UWeaponDataAsset* WeaponData = Equipment ? Equipment->GetCurrentWeaponData() : nullptr;

	if (!WeaponData)
	{
		SetTextIfChanged(WeaponText, WeaponTextCache, TEXT("无武器"));
		SetTextIfChanged(AmmoText, AmmoTextCache, TEXT("— / —"));
		return;
	}

	SetTextIfChanged(WeaponText, WeaponTextCache, WeaponData->WeaponName.ToString());

	// 换弹倒计时是时间戳派生值(服务器读 Timer,客户端读 ReloadEndServerTime),必须逐帧算。
	const FString AmmoLine = Equipment->IsReloading()
		? FString::Printf(TEXT("换弹中 %.1fs"), Equipment->GetReloadTimeRemaining())
		: FString::Printf(TEXT("%d / %d   储备 %d"),
			Equipment->GetCurrentMagazineAmmo(),
			Equipment->GetCurrentMagazineSize(),
			Equipment->GetCurrentReserveAmmo());
	SetTextIfChanged(AmmoText, AmmoTextCache, AmmoLine);
}

void UWeaponCombatHUDWidget::RefreshGrenadeCooldown()
{
	const UAbilitySystemComponent* ASC = BoundASC.Get();
	if (!ASC || !GrenadeText)
	{
		return;
	}

	// 冷却 GE 授予 Cooldown.Weapon.Grenade;Mixed 模式下 GE 复制给拥有者客户端,可直接查剩余时间。
	const FGameplayEffectQuery CooldownQuery = FGameplayEffectQuery::MakeQuery_MatchAnyOwningTags(
		FGameplayTagContainer(TAG_Cooldown_Weapon_Grenade));
	float Remaining = 0.0f;
	for (const TPair<float, float>& TimePair : ASC->GetActiveEffectsTimeRemainingAndDuration(CooldownQuery))
	{
		Remaining = FMath::Max(Remaining, TimePair.Key);
	}

	const bool bReady = Remaining <= 0.0f;
	const FString Line = bReady ? FString(TEXT("手雷 就绪")) : FString::Printf(TEXT("手雷 冷却 %.1fs"), Remaining);
	if (Line != GrenadeTextCache)
	{
		GrenadeText->SetColorAndOpacity(FSlateColor(bReady ? CombatHUDStyle::ReadyColor : CombatHUDStyle::LabelColor));
	}
	SetTextIfChanged(GrenadeText, GrenadeTextCache, Line);
}

void UWeaponCombatHUDWidget::RefreshRespawn()
{
	const AWeaponPlayerState* PS = GetOwningPlayerState<AWeaponPlayerState>();
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	const float RespawnServerTime = PS ? PS->GetRespawnServerTime() : 0.0f;
	if (!RespawnText || !WeaponGameState || RespawnServerTime <= 0.0f)
	{
		if (RespawnText && RespawnText->GetVisibility() != ESlateVisibility::Collapsed)
		{
			RespawnText->SetVisibility(ESlateVisibility::Collapsed);
		}
		return;
	}

	const float Remaining = FMath::Max(0.0f, RespawnServerTime - WeaponGameState->GetServerWorldTimeSeconds());
	SetTextIfChanged(RespawnText, RespawnTextCache, FString::Printf(TEXT("已倒下 · %.1f 秒后复活"), Remaining));
	if (RespawnText->GetVisibility() != ESlateVisibility::HitTestInvisible)
	{
		RespawnText->SetVisibility(ESlateVisibility::HitTestInvisible);
	}
}

void UWeaponCombatHUDWidget::RefreshPhaseVisibility()
{
	const AWeaponGameState* WeaponGameState = GetWorld() ? GetWorld()->GetGameState<AWeaponGameState>() : nullptr;
	const bool bShouldShow = WeaponGameState && WeaponGameState->GetMatchPhase() == EWeaponMatchPhase::InGame;
	if (bShouldShow == bHUDVisible || !HUDCanvas)
	{
		return;
	}
	bHUDVisible = bShouldShow;
	HUDCanvas->SetVisibility(bShouldShow ? ESlateVisibility::HitTestInvisible : ESlateVisibility::Collapsed);
}

void UWeaponCombatHUDWidget::SetTextIfChanged(UTextBlock* TextBlock, FString& Cache, const FString& NewText)
{
	if (!TextBlock || Cache == NewText)
	{
		return;
	}
	Cache = NewText;
	TextBlock->SetText(FText::FromString(NewText));
}
