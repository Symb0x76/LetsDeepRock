// Copyright Epic Games, Inc. All Rights Reserved.

#include "WeaponHUD.h"
#include "Attributes/WeaponHealthSet.h"
#include "Data/GameDataSubsystem.h"
#include "EnemyCharacter.h"
#include "WeaponCharacter.h"
#include "Weapons/WeaponDataAsset.h"
#include "Weapons/WeaponEquipmentComponent.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "Engine/Font.h"
#include "EngineUtils.h"
#include "GameFramework/PlayerController.h"

namespace WeaponHUDPrivate
{
	static void DrawLine(UCanvas* Canvas, UFont* Font, const FString& Text, float X, float Y,
		const FLinearColor& Color = FLinearColor::White, bool bCentre = false)
	{
		Canvas->K2_DrawText(Font, Text, FVector2D(X, Y), FVector2D(1.0f), Color, 0.0f,
			FLinearColor::Black, FVector2D(1.0f), bCentre, false, true, FLinearColor(0.0f, 0.0f, 0.0f, 0.8f));
	}

	static FLinearColor GetDamageColor(FName DamageType)
	{
		const FString Type = DamageType.ToString();
		if (Type.EndsWith(TEXT("Physical"))) return FLinearColor::White;
		if (Type.EndsWith(TEXT("Fire"))) return FLinearColor(1.0f, 0.35f, 0.05f);
		if (Type.EndsWith(TEXT("Explosion"))) return FLinearColor::Yellow;
		if (Type.EndsWith(TEXT("Bleed"))) return FLinearColor(0.55f, 0.02f, 0.02f);
		if (Type.EndsWith(TEXT("Vulnerable"))) return FLinearColor(0.65f, 0.15f, 1.0f);
		return FLinearColor(0.45f, 0.85f, 1.0f);
	}
}

void AWeaponHUD::DrawHUD()
{
	Super::DrawHUD();
	if (!Canvas)
	{
		return;
	}

	// 蓄力条是玩法 UI，不受 Weapon Debug UI 开关影响，始终按蓄力状态绘制；
	// 其余调试面板仍受 IsWeaponDebugUIEnabled 控制。
	const bool bDebugUIEnabled = IsWeaponDebugUIEnabled();
	if (bDebugUIEnabled)
	{
		DrawCurrentWeaponInfo();
		DrawNPCDebugInfo();
		DrawDamageEvents();
		DrawLastDamageDetails();
		DrawAmmoPanel();
	}
	DrawChargeFireBar(bDebugUIEnabled);
}

void AWeaponHUD::AddWeaponDamageDebugEvent(const FWeaponDamageDebugEvent& Event)
{
#if !UE_BUILD_SHIPPING
	if (!IsWeaponDebugUIEnabled() || !GetWorld())
	{
		return;
	}

	FActiveDamageNumber& Number = ActiveDamageNumbers.AddDefaulted_GetRef();
	Number.Event = Event;
	Number.StartTime = GetWorld()->GetTimeSeconds();
	if (ActiveDamageNumbers.Num() > 64)
	{
		ActiveDamageNumbers.RemoveAt(0, ActiveDamageNumbers.Num() - 64);
	}
	LastDamageEvent = Event;
	bHasLastDamageEvent = true;
#endif
}

void AWeaponHUD::DrawCurrentWeaponInfo()
{
	UWeaponEquipmentComponent* EquipComp = nullptr;
	if (APawn* OwningPawn = GetOwningPawn())
	{
		EquipComp = OwningPawn->FindComponentByClass<UWeaponEquipmentComponent>();
	}

	const UWeaponDataAsset* WeaponData = EquipComp ? EquipComp->GetCurrentWeaponData() : nullptr;
	const int32 WeaponIndex = EquipComp ? EquipComp->GetCurrentWeaponIndex() : INDEX_NONE;
	const FString SlotName = WeaponIndex == INDEX_NONE ? TEXT("未装备")
		: (WeaponIndex == 0 ? TEXT("主武器") : (WeaponIndex == 1 ? TEXT("副武器") : TEXT("扩展槽位")));
	const FWeaponConfigRow* WeaponConfig = nullptr;
	const FWeaponFireModeConfigRow* FireModeConfig = nullptr;
	if (WeaponData && !WeaponData->WeaponConfigId.IsNone())
	{
		if (UGameInstance* GameInstance = GetGameInstance())
		{
			if (UGameDataSubsystem* DataSys = GameInstance->GetSubsystem<UGameDataSubsystem>())
			{
				WeaponConfig = DataSys->GetWeaponConfig(WeaponData->WeaponConfigId);
				if (WeaponConfig)
				{
					FireModeConfig = DataSys->GetWeaponFireModeConfig(EquipComp->GetCurrentFireModeId());
				}
			}
		}
	}

	FString DisplayName = TEXT("未装备武器");
	if (WeaponData && !WeaponData->WeaponName.IsEmpty())
	{
		DisplayName = WeaponData->WeaponName.ToString();
	}
	else if (WeaponConfig && !WeaponConfig->DisplayName.IsEmpty())
	{
		DisplayName = WeaponConfig->DisplayName.ToString();
	}

	FString DamageType = TEXT("N/A");
	if (FireModeConfig && FireModeConfig->DamageTypeTag.IsValid())
	{
		DamageType = FireModeConfig->DamageTypeTag.ToString();
	}
	else if (WeaponData)
	{
		const FGameplayTag Root = FGameplayTag::RequestGameplayTag(TEXT("Damage.Type"), false);
		for (const FGameplayTag& Tag : WeaponData->DamageTags)
		{
			if (Root.IsValid() && Tag.MatchesTag(Root) && Tag != Root)
			{
				DamageType = Tag.ToString();
				break;
			}
		}
	}

	TArray<FString> Lines;
	Lines.Add(FString::Printf(TEXT("当前武器: %s"), *DisplayName));
	Lines.Add(FString::Printf(TEXT("ConfigId: %s"), WeaponData ? *WeaponData->WeaponConfigId.ToString() : TEXT("N/A")));
	Lines.Add(FString::Printf(TEXT("索引/槽位: %d / %s"), WeaponIndex, *SlotName));
	Lines.Add(FString::Printf(TEXT("当前模式: %s"), EquipComp ? *EquipComp->GetCurrentFireModeId().ToString() : TEXT("N/A")));
	Lines.Add(FString::Printf(TEXT("DamageType: %s"), *DamageType));
	const FString BaseDamageText = FireModeConfig ? FString::SanitizeFloat(FireModeConfig->BaseDamage)
		: (WeaponData ? FString::SanitizeFloat(WeaponData->BaseDamage) : TEXT("N/A"));
	const FString ProjectileSpeedText = FireModeConfig ? FString::SanitizeFloat(FireModeConfig->ProjectileSpeed)
		: (WeaponData ? FString::SanitizeFloat(WeaponData->ProjectileSpeed) : TEXT("N/A"));
	const FString LifetimeText = FireModeConfig ? FString::SanitizeFloat(FireModeConfig->ProjectileLifetime) : TEXT("N/A");
	const FString GravityText = FireModeConfig ? FString::SanitizeFloat(FireModeConfig->ProjectileGravityScale) : TEXT("N/A");
	const FString MaxHitTargetsText = FireModeConfig ? FString::FromInt(FireModeConfig->MaxHitTargets) : TEXT("N/A");
	Lines.Add(FString::Printf(TEXT("BaseDamage: %s"), *BaseDamageText));
	Lines.Add(FString::Printf(TEXT("ProjectileSpeed: %s"), *ProjectileSpeedText));
	Lines.Add(FString::Printf(TEXT("Lifetime: %s"), *LifetimeText));
	Lines.Add(FString::Printf(TEXT("Gravity: %s"), *GravityText));
	Lines.Add(FString::Printf(TEXT("MaxHitTargets: %s"), *MaxHitTargetsText));

	DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.6f), 16.0f, 16.0f, 390.0f, 198.0f);
	UFont* Font = GEngine->GetSmallFont();
	for (int32 Index = 0; Index < Lines.Num(); ++Index)
	{
		WeaponHUDPrivate::DrawLine(Canvas, Font, Lines[Index], 24.0f, 22.0f + Index * 18.0f,
			Index == 0 ? FLinearColor(0.3f, 0.85f, 1.0f) : FLinearColor::White);
	}
}

void AWeaponHUD::DrawNPCDebugInfo()
{
	const APawn* OwningPawn = GetOwningPawn();
	if (!OwningPawn || !PlayerOwner)
	{
		return;
	}

	UFont* Font = GEngine->GetSmallFont();
	for (TActorIterator<AEnemyCharacter> It(GetWorld()); It; ++It)
	{
		AEnemyCharacter* NPC = *It;
		if (FVector::DistSquared(OwningPawn->GetActorLocation(), NPC->GetActorLocation()) > FMath::Square(5000.0f))
		{
			continue;
		}

		FVector2D Screen;
		const FVector HeadLocation = NPC->GetActorLocation() + FVector(0.0f, 0.0f, NPC->GetSimpleCollisionHalfHeight() + 45.0f);
		if (!PlayerOwner->ProjectWorldLocationToScreen(HeadLocation, Screen, true)
			|| Screen.X < 0.0f || Screen.Y < 0.0f || Screen.X > Canvas->ClipX || Screen.Y > Canvas->ClipY)
		{
			continue;
		}

		const UWeaponHealthSet* HealthSet = NPC->GetHealthSet();
		if (!HealthSet)
		{
			continue;
		}

		const float Health = HealthSet->GetHealth();
		const float MaxHealth = HealthSet->GetMaxHealth();
		const float Shield = HealthSet->GetShield();
		const float MaxShield = HealthSet->GetMaxShield();
		const bool bDead = Health <= 0.0f;
		const FString Header = FString::Printf(TEXT("%s  HP %.0f/%.0f  Shield %.0f/%.0f%s"),
			*NPC->GetEnemyConfigRowName().ToString(), Health, MaxHealth, Shield, MaxShield, bDead ? TEXT("  DEAD") : TEXT(""));
		WeaponHUDPrivate::DrawLine(Canvas, Font, Header, Screen.X, Screen.Y, bDead ? FLinearColor::Red : FLinearColor::White, true);

		const float BarWidth = 120.0f;
		const float BarX = Screen.X - BarWidth * 0.5f;
		DrawRect(FLinearColor(0.05f, 0.05f, 0.05f, 0.85f), BarX, Screen.Y + 16.0f, BarWidth, 7.0f);
		DrawRect(FLinearColor(0.8f, 0.08f, 0.05f), BarX, Screen.Y + 16.0f,
			BarWidth * FMath::Clamp(MaxHealth > 0.0f ? Health / MaxHealth : 0.0f, 0.0f, 1.0f), 7.0f);
		DrawRect(FLinearColor(0.05f, 0.05f, 0.05f, 0.85f), BarX, Screen.Y + 25.0f, BarWidth, 5.0f);
		DrawRect(FLinearColor(0.1f, 0.55f, 1.0f), BarX, Screen.Y + 25.0f,
			BarWidth * FMath::Clamp(MaxShield > 0.0f ? Shield / MaxShield : 0.0f, 0.0f, 1.0f), 5.0f);
	}
}

void AWeaponHUD::DrawDamageEvents()
{
	const float Now = GetWorld()->GetTimeSeconds();
	UFont* Font = GEngine->GetMediumFont();
	for (int32 Index = ActiveDamageNumbers.Num() - 1; Index >= 0; --Index)
	{
		const float Age = Now - ActiveDamageNumbers[Index].StartTime;
		if (Age >= 1.25f)
		{
			ActiveDamageNumbers.RemoveAtSwap(Index);
			continue;
		}

		FVector2D Screen;
		if (!PlayerOwner || !PlayerOwner->ProjectWorldLocationToScreen(ActiveDamageNumbers[Index].Event.TargetLocation, Screen, true))
		{
			continue;
		}
		Screen.Y -= 35.0f + Age * 45.0f;
		if (Screen.X < 0.0f || Screen.Y < 0.0f || Screen.X > Canvas->ClipX || Screen.Y > Canvas->ClipY)
		{
			continue;
		}

		FLinearColor Color = WeaponHUDPrivate::GetDamageColor(ActiveDamageNumbers[Index].Event.DamageType);
		Color.A = 1.0f - Age / 1.25f;
		WeaponHUDPrivate::DrawLine(Canvas, Font,
			FString::Printf(TEXT("-%.1f"), ActiveDamageNumbers[Index].Event.FinalDamage), Screen.X, Screen.Y, Color, true);
	}
}

void AWeaponHUD::DrawLastDamageDetails()
{
	if (!bHasLastDamageEvent)
	{
		return;
	}

	const float X = Canvas->ClipX - 330.0f;
	const float Y = 24.0f;
	DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.6f), X - 10.0f, Y - 8.0f, 316.0f, 174.0f);
	const TArray<FString> Lines = {
		TEXT("最近一次伤害"),
		FString::Printf(TEXT("实际总伤: %.1f"), LastDamageEvent.FinalDamage),
		FString::Printf(TEXT("盾伤 / 生命伤: %.1f / %.1f"), LastDamageEvent.ShieldDamage, LastDamageEvent.HealthDamage),
		FString::Printf(TEXT("类型: %s"), *LastDamageEvent.DamageType.ToString()),
		FString::Printf(TEXT("暴击: %s  弱点: %s"), LastDamageEvent.bCritical ? TEXT("是") : TEXT("否"), LastDamageEvent.bWeakpoint ? TEXT("是") : TEXT("否")),
		FString::Printf(TEXT("距离: %.2f m"), LastDamageEvent.HitDistanceMeters),
		FString::Printf(TEXT("目标: %s"), *LastDamageEvent.TargetName)
	};
	UFont* Font = GEngine->GetSmallFont();
	for (int32 Index = 0; Index < Lines.Num(); ++Index)
	{
		WeaponHUDPrivate::DrawLine(Canvas, Font, Lines[Index], X, Y + Index * 22.0f,
			Index == 0 ? FLinearColor(1.0f, 0.8f, 0.2f) : FLinearColor::White);
	}
}

void AWeaponHUD::DrawAmmoPanel()
{
	UWeaponEquipmentComponent* EquipComp = nullptr;
	if (APawn* OwningPawn = GetOwningPawn())
	{
		EquipComp = OwningPawn->FindComponentByClass<UWeaponEquipmentComponent>();
	}
	if (!EquipComp || !EquipComp->GetCurrentWeaponData())
	{
		return;
	}

	const int32 MagazineAmmo = EquipComp->GetCurrentMagazineAmmo();
	const int32 MagazineSize = EquipComp->GetCurrentMagazineSize();
	const int32 ReserveAmmo = EquipComp->GetCurrentReserveAmmo();
	const bool bReloading = EquipComp->IsReloading();
	const float ReloadRemaining = EquipComp->GetReloadTimeRemaining();

	// 面板尺寸与右下角锚定：随分辨率自适应，与右上角伤害面板互不影响（后者贴顶部）。
	const float PanelWidth = 220.0f;
	const float PanelHeight = bReloading ? 122.0f : 100.0f;
	const float Margin = 20.0f;
	const float PanelX = Canvas->ClipX - PanelWidth - Margin;
	const float PanelY = Canvas->ClipY - PanelHeight - Margin;

	DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.62f), PanelX, PanelY, PanelWidth, PanelHeight);
	DrawRect(FLinearColor(1.0f, 0.8f, 0.2f, 0.9f), PanelX, PanelY, PanelWidth, 3.0f);

	UFont* SmallFont = GEngine->GetSmallFont();
	UFont* MediumFont = GEngine->GetMediumFont();
	const float InnerX = PanelX + 14.0f;
	float CursorY = PanelY + 10.0f;

	// 标题行
	WeaponHUDPrivate::DrawLine(Canvas, SmallFont, TEXT("弹药测试"), InnerX, CursorY, FLinearColor(1.0f, 0.8f, 0.2f));
	CursorY += 20.0f;

	// 弹匣 / 容量：大号数字，空弹匣时警示为红色
	const bool bMagazineEmpty = MagazineAmmo <= 0 && MagazineSize > 0;
	const FLinearColor MagazineColor = bReloading ? FLinearColor(0.6f, 0.6f, 0.6f)
		: (bMagazineEmpty ? FLinearColor(1.0f, 0.25f, 0.2f) : FLinearColor::White);
	const FString MagazineText = FString::Printf(TEXT("%d / %d"), MagazineAmmo, MagazineSize);
	WeaponHUDPrivate::DrawLine(Canvas, MediumFont, MagazineText, InnerX, CursorY, MagazineColor);
	CursorY += 26.0f;

	// 备弹行
	WeaponHUDPrivate::DrawLine(Canvas, SmallFont, FString::Printf(TEXT("备弹: %d"), ReserveAmmo), InnerX, CursorY, FLinearColor(0.85f, 0.85f, 0.85f));
	CursorY += 20.0f;

	if (bReloading)
	{
		// 换弹中：黄色提示文案 + 剩余秒数
		WeaponHUDPrivate::DrawLine(Canvas, SmallFont, FString::Printf(TEXT("换弹中... 剩余 %.1f 秒"), ReloadRemaining),
			InnerX, CursorY, FLinearColor(1.0f, 0.75f, 0.1f));
		CursorY += 22.0f;

		// 底部做一条低调的扫描动画条，仅作"进行中"视觉提示，不代表精确进度
		// （GetReloadTimeRemaining 只提供剩余秒数，UI 层不引入总时长查询接口）。
		const float BarWidth = PanelWidth - 28.0f;
		const float BarX = InnerX;
		const float BarY = CursorY;
		DrawRect(FLinearColor(0.05f, 0.05f, 0.05f, 0.85f), BarX, BarY, BarWidth, 6.0f);
		if (GetWorld())
		{
			const float ScanWidth = BarWidth * 0.28f;
			const float Phase = FMath::Fmod(GetWorld()->GetTimeSeconds() * 1.6f, 1.0f);
			const float ScanX = BarX + (BarWidth - ScanWidth) * Phase;
			DrawRect(FLinearColor(1.0f, 0.75f, 0.1f, 0.95f), ScanX, BarY, ScanWidth, 6.0f);
		}
	}
	else
	{
		WeaponHUDPrivate::DrawLine(Canvas, SmallFont, TEXT("就绪"), InnerX, CursorY, FLinearColor(0.3f, 0.9f, 0.35f));
	}
}

void AWeaponHUD::DrawChargeFireBar(bool bDebugUIEnabled)
{
	const AWeaponCharacter* WeaponPawn = Cast<AWeaponCharacter>(GetOwningPawn());
	if (!WeaponPawn || !WeaponPawn->IsChargingFire())
	{
		return;
	}

	const float Progress = FMath::Clamp(WeaponPawn->GetChargeFireProgress(), 0.0f, 1.0f);
	const bool bFullyCharged = Progress >= 1.0f;

	// 尺寸/位置随分辨率自适应，右下角安全边距。
	const float BarWidth = 220.0f;
	const float BarHeight = 16.0f;
	const float Margin = 20.0f;
	const float BarX = Canvas->ClipX - BarWidth - Margin;

	// Debug 面板开启时让位到弹药面板（DrawAmmoPanel）上方，避免遮挡；
	// 关闭时蓄力条是唯一的右下角玩法 UI，直接贴近角落，只保留安全边距。
	float BarY;
	if (bDebugUIEnabled)
	{
		const float AmmoPanelHeight = 122.0f; // 与 DrawAmmoPanel 换弹中状态高度一致，取较大值保证不重叠
		BarY = Canvas->ClipY - AmmoPanelHeight - Margin - BarHeight - 34.0f;
	}
	else
	{
		BarY = Canvas->ClipY - BarHeight - Margin;
	}

	// 满蓄力时边框/填充变为亮橙色，作为"可以松手释放"的明确提示。
	const FLinearColor FillColor = bFullyCharged ? FLinearColor(1.0f, 0.55f, 0.05f) : FLinearColor(1.0f, 0.85f, 0.25f);
	const FLinearColor LabelColor = bFullyCharged ? FLinearColor(1.0f, 0.6f, 0.1f) : FLinearColor(1.0f, 0.9f, 0.5f);

	UFont* SmallFont = GEngine->GetSmallFont();
	WeaponHUDPrivate::DrawLine(Canvas, SmallFont, bFullyCharged ? TEXT("蓄力完毕") : TEXT("蓄力中"),
		BarX, BarY - 18.0f, LabelColor);

	DrawRect(FLinearColor(0.0f, 0.0f, 0.0f, 0.7f), BarX, BarY, BarWidth, BarHeight);
	DrawRect(FLinearColor(0.05f, 0.05f, 0.05f, 0.85f), BarX + 2.0f, BarY + 2.0f, BarWidth - 4.0f, BarHeight - 4.0f);
	DrawRect(FillColor, BarX + 2.0f, BarY + 2.0f, (BarWidth - 4.0f) * Progress, BarHeight - 4.0f);

	// 满蓄力时边缘描一层高亮边框，进一步强化"已就绪"的视觉反馈。
	if (bFullyCharged)
	{
		DrawRect(FLinearColor(1.0f, 0.85f, 0.3f, 0.9f), BarX, BarY, BarWidth, 2.0f);
		DrawRect(FLinearColor(1.0f, 0.85f, 0.3f, 0.9f), BarX, BarY + BarHeight - 2.0f, BarWidth, 2.0f);
	}
}
