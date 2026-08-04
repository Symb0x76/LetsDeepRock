// 职业装备与护甲选择存档

#pragma once

#include "CoreMinimal.h"
#include "GameFramework/SaveGame.h"
#include "ProfessionSaveGame.generated.h"

UCLASS(BlueprintType)
class LETSDEEPROCK_API UProfessionSaveGame : public USaveGame
{
	GENERATED_BODY()

public:
	/** 选择职业与主副武器，并将配置保存到固定槽位 */
	UFUNCTION(BlueprintCallable, Category = "Profession|Save", meta = (WorldContext = "WorldContextObject"))
	static bool SelectProfession(const UObject* WorldContextObject, FName ProfessionId, FName PrimaryWeaponId, FName SecondaryWeaponId);

	/** 纯校验职业与主副武器组合，不读写存档。 */
	static bool IsProfessionSelectionValid(const UObject* WorldContextObject, FName ProfessionId, FName PrimaryWeaponId, FName SecondaryWeaponId);

	UPROPERTY(BlueprintReadWrite, SaveGame, Category = "Profession")
	FName SelectedProfession;

	UPROPERTY(BlueprintReadWrite, SaveGame, Category = "Profession|Loadout")
	FName PrimaryWeaponId;

	UPROPERTY(BlueprintReadWrite, SaveGame, Category = "Profession|Loadout")
	FName SecondaryWeaponId;

	UPROPERTY(BlueprintReadWrite, SaveGame, Category = "Profession|Loadout")
	FName SkillItemId;

	UPROPERTY(BlueprintReadWrite, SaveGame, Category = "Profession|Loadout")
	FName ClassEquipmentId;

	UPROPERTY(BlueprintReadOnly, SaveGame, Category = "Profession|Armor")
	TArray<FName> SelectedArmorPerkIds;

	/** 将当前对象保存到固定槽位 */
	UFUNCTION(BlueprintCallable, Category = "Profession|Save", meta = (WorldContext = "WorldContextObject"))
	static bool SaveCurrentConfiguration(const UObject* WorldContextObject, UProfessionSaveGame* Configuration);

	/** 从固定槽位加载；没有存档时返回一个空配置 */
	UFUNCTION(BlueprintCallable, Category = "Profession|Save")
	static UProfessionSaveGame* LoadCurrentConfiguration();

	/** 校验并设置护甲选择；候选为空时仅接受空选择 */
	UFUNCTION(BlueprintCallable, Category = "Profession|Armor", meta = (WorldContext = "WorldContextObject"))
	bool SetSelectedArmorPerks(const UObject* WorldContextObject, const TArray<FName>& NewArmorPerkIds);

private:
	bool IsArmorPerkSelectionValid(const UObject* WorldContextObject, const TArray<FName>& ArmorPerkIds) const;

	static const FString SaveSlotName;
};
