#include "ProfessionSaveGame.h"

#include "GameDataSubsystem.h"
#include "Engine/GameInstance.h"
#include "Kismet/GameplayStatics.h"

const FString UProfessionSaveGame::SaveSlotName = TEXT("ProfessionConfiguration");

bool UProfessionSaveGame::SelectProfession(const UObject* WorldContextObject, FName ProfessionId, FName PrimaryWeaponId, FName SecondaryWeaponId)
{
	if (!IsProfessionSelectionValid(WorldContextObject, ProfessionId, PrimaryWeaponId, SecondaryWeaponId)) return false;

	UGameInstance* GameInstance = UGameplayStatics::GetGameInstance(WorldContextObject);
	UGameDataSubsystem* DataSubsystem = GameInstance ? GameInstance->GetSubsystem<UGameDataSubsystem>() : nullptr;
	const FProfessionConfigRow* ProfessionRow = DataSubsystem ? DataSubsystem->GetProfessionConfig(ProfessionId) : nullptr;

	UProfessionSaveGame* Configuration = LoadCurrentConfiguration();
	if (!Configuration) return false;

	Configuration->SelectedProfession = ProfessionId;
	Configuration->PrimaryWeaponId = PrimaryWeaponId;
	Configuration->SecondaryWeaponId = SecondaryWeaponId;
	Configuration->SkillItemId = ProfessionRow->SkillItemId;
	Configuration->ClassEquipmentId = ProfessionRow->ClassEquipmentId;
	Configuration->SelectedArmorPerkIds.Reset();

	return SaveCurrentConfiguration(WorldContextObject, Configuration);
}

bool UProfessionSaveGame::IsProfessionSelectionValid(const UObject* WorldContextObject, FName ProfessionId, FName PrimaryWeaponId, FName SecondaryWeaponId)
{
	UGameInstance* GameInstance = UGameplayStatics::GetGameInstance(WorldContextObject);
	UGameDataSubsystem* DataSubsystem = GameInstance ? GameInstance->GetSubsystem<UGameDataSubsystem>() : nullptr;
	return DataSubsystem
		&& DataSubsystem->GetProfessionConfig(ProfessionId)
		&& DataSubsystem->GetProfessionWeaponIds(ProfessionId, TEXT("Primary")).Contains(PrimaryWeaponId)
		&& DataSubsystem->GetProfessionWeaponIds(ProfessionId, TEXT("Secondary")).Contains(SecondaryWeaponId);
}

bool UProfessionSaveGame::SaveCurrentConfiguration(const UObject* WorldContextObject, UProfessionSaveGame* Configuration)
{
	return Configuration
		&& Configuration->IsArmorPerkSelectionValid(WorldContextObject, Configuration->SelectedArmorPerkIds)
		&& UGameplayStatics::SaveGameToSlot(Configuration, SaveSlotName, 0);
}

UProfessionSaveGame* UProfessionSaveGame::LoadCurrentConfiguration()
{
	if (USaveGame* LoadedGame = UGameplayStatics::LoadGameFromSlot(SaveSlotName, 0))
	{
		if (UProfessionSaveGame* Configuration = Cast<UProfessionSaveGame>(LoadedGame))
		{
			return Configuration;
		}
	}

	return Cast<UProfessionSaveGame>(UGameplayStatics::CreateSaveGameObject(StaticClass()));
}

bool UProfessionSaveGame::SetSelectedArmorPerks(const UObject* WorldContextObject, const TArray<FName>& NewArmorPerkIds)
{
	if (!IsArmorPerkSelectionValid(WorldContextObject, NewArmorPerkIds)) return false;

	SelectedArmorPerkIds = NewArmorPerkIds;
	return true;
}

bool UProfessionSaveGame::IsArmorPerkSelectionValid(const UObject* WorldContextObject, const TArray<FName>& ArmorPerkIds) const
{
	UGameInstance* GameInstance = UGameplayStatics::GetGameInstance(WorldContextObject);
	UGameDataSubsystem* DataSubsystem = GameInstance ? GameInstance->GetSubsystem<UGameDataSubsystem>() : nullptr;
	if (!DataSubsystem) return false;

	const TArray<FName> CandidatePerks = DataSubsystem->GetProfessionArmorPerkIds(SelectedProfession);
	if (CandidatePerks.IsEmpty()) return ArmorPerkIds.IsEmpty();
	if (ArmorPerkIds.Num() > 2 || (CandidatePerks.Num() == 5 && ArmorPerkIds.Num() != 2)) return false;

	TArray<FName> ValidatedPerks;
	for (const FName PerkId : ArmorPerkIds)
	{
		if (PerkId.IsNone() || !CandidatePerks.Contains(PerkId) || ValidatedPerks.Contains(PerkId)) return false;
		ValidatedPerks.Add(PerkId);
	}

	return true;
}
