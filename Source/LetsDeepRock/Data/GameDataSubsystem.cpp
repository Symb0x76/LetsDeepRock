#include "GameDataSubsystem.h"
#include "Engine/DataTable.h"

void UGameDataSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// 加载 7 张装备配置 DataTable：Profession、Weapon、WeaponFireMode、Grenade、StatusEffect、Tool、Drone
	// 路径格式: /Game/Data/资产名.资产名
	// 如果资产不存在（尚未在编辑器中创建），返回 nullptr 不会崩溃
	EnemyConfigTable = LoadObject<UDataTable>(nullptr,
		TEXT("/Game/Data/DT_EnemyConfig.DT_EnemyConfig"));
	SpawnerConfigTable = LoadObject<UDataTable>(nullptr,
		TEXT("/Game/Data/DT_SpawnerConfig.DT_SpawnerConfig"));
	GameModeConfigTable = LoadObject<UDataTable>(nullptr,
		TEXT("/Game/Data/DT_GameModeConfig.DT_GameModeConfig"));
	CharacterAttributeTable = LoadObject<UDataTable>(nullptr,
		TEXT("/Game/Data/DT_CharacterAttributes.DT_CharacterAttributes"));
	ProfessionConfigTable = LoadObject<UDataTable>(nullptr,
		TEXT("/Game/Data/DT_ProfessionConfig.DT_ProfessionConfig"));
	WeaponConfigTable = LoadObject<UDataTable>(nullptr,
		TEXT("/Game/Data/DT_WeaponConfig.DT_WeaponConfig"));
	WeaponFireModeConfigTable = LoadObject<UDataTable>(nullptr,
		TEXT("/Game/Data/DT_WeaponFireModeConfig.DT_WeaponFireModeConfig"));
	GrenadeConfigTable = LoadObject<UDataTable>(nullptr,
		TEXT("/Game/Data/DT_GrenadeConfig.DT_GrenadeConfig"));
	StatusEffectConfigTable = LoadObject<UDataTable>(nullptr,
		TEXT("/Game/Data/DT_StatusEffectConfig.DT_StatusEffectConfig"));
	ToolConfigTable = LoadObject<UDataTable>(nullptr,
		TEXT("/Game/Data/DT_ToolConfig.DT_ToolConfig"));
	DroneConfigTable = LoadObject<UDataTable>(nullptr,
		TEXT("/Game/Data/DT_DroneConfig.DT_DroneConfig"));

	if (!EnemyConfigTable)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GameDataSubsystem] DT_EnemyConfig not found at /Game/Data/. DataTable-driven values will use defaults."));
	}
	if (!SpawnerConfigTable)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GameDataSubsystem] DT_SpawnerConfig not found at /Game/Data/. DataTable-driven values will use defaults."));
	}
	if (!GameModeConfigTable)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GameDataSubsystem] DT_GameModeConfig not found at /Game/Data/. DataTable-driven values will use defaults."));
	}
	if (!CharacterAttributeTable)
	{
		UE_LOG(LogTemp, Warning, TEXT("[GameDataSubsystem] DT_CharacterAttributes not found at /Game/Data/. DataTable-driven values will use defaults."));
	}
	if (!ProfessionConfigTable) UE_LOG(LogTemp, Warning, TEXT("[GameDataSubsystem] DT_ProfessionConfig not found at /Game/Data/."));
	if (!WeaponConfigTable) UE_LOG(LogTemp, Warning, TEXT("[GameDataSubsystem] DT_WeaponConfig not found at /Game/Data/."));
	if (!WeaponFireModeConfigTable) UE_LOG(LogTemp, Warning, TEXT("[GameDataSubsystem] DT_WeaponFireModeConfig not found at /Game/Data/."));
	if (!GrenadeConfigTable) UE_LOG(LogTemp, Warning, TEXT("[GameDataSubsystem] DT_GrenadeConfig not found at /Game/Data/."));
	if (!StatusEffectConfigTable) UE_LOG(LogTemp, Warning, TEXT("[GameDataSubsystem] DT_StatusEffectConfig not found at /Game/Data/."));
	if (!ToolConfigTable) UE_LOG(LogTemp, Warning, TEXT("[GameDataSubsystem] DT_ToolConfig not found at /Game/Data/."));
	if (!DroneConfigTable) UE_LOG(LogTemp, Warning, TEXT("[GameDataSubsystem] DT_DroneConfig not found at /Game/Data/."));
}

const FEnemyConfigRow* UGameDataSubsystem::GetEnemyConfig(FName RowName) const
{
	if (!EnemyConfigTable) return nullptr;
	return EnemyConfigTable->FindRow<FEnemyConfigRow>(RowName, TEXT("GetEnemyConfig"));
}

const FSpawnerConfigRow* UGameDataSubsystem::GetSpawnerConfig(FName RowName) const
{
	if (!SpawnerConfigTable) return nullptr;
	return SpawnerConfigTable->FindRow<FSpawnerConfigRow>(RowName, TEXT("GetSpawnerConfig"));
}

const FGameModeConfigRow* UGameDataSubsystem::GetGameModeConfig(FName RowName) const
{
	if (!GameModeConfigTable) return nullptr;
	return GameModeConfigTable->FindRow<FGameModeConfigRow>(RowName, TEXT("GetGameModeConfig"));
}

const FCharacterAttributeRow* UGameDataSubsystem::GetCharacterAttributes(FName RowName) const
{
	if (!CharacterAttributeTable) return nullptr;
	return CharacterAttributeTable->FindRow<FCharacterAttributeRow>(RowName, TEXT("GetCharacterAttributes"));
}

const FProfessionConfigRow* UGameDataSubsystem::GetProfessionConfig(FName RowName) const
{
	if (!ProfessionConfigTable) return nullptr;
	return ProfessionConfigTable->FindRow<FProfessionConfigRow>(RowName, TEXT("GetProfessionConfig"));
}

const FWeaponConfigRow* UGameDataSubsystem::GetWeaponConfig(FName RowName) const
{
	if (!WeaponConfigTable) return nullptr;
	return WeaponConfigTable->FindRow<FWeaponConfigRow>(RowName, TEXT("GetWeaponConfig"));
}

const FWeaponFireModeConfigRow* UGameDataSubsystem::GetWeaponFireModeConfig(FName RowName) const
{
	if (!WeaponFireModeConfigTable) return nullptr;
	return WeaponFireModeConfigTable->FindRow<FWeaponFireModeConfigRow>(RowName, TEXT("GetWeaponFireModeConfig"));
}

const FGrenadeConfigRow* UGameDataSubsystem::GetGrenadeConfig(FName RowName) const
{
	if (!GrenadeConfigTable) return nullptr;
	return GrenadeConfigTable->FindRow<FGrenadeConfigRow>(RowName, TEXT("GetGrenadeConfig"));
}

const FStatusEffectConfigRow* UGameDataSubsystem::GetStatusEffectConfig(FName RowName) const
{
	if (!StatusEffectConfigTable) return nullptr;
	return StatusEffectConfigTable->FindRow<FStatusEffectConfigRow>(RowName, TEXT("GetStatusEffectConfig"));
}

const FToolConfigRow* UGameDataSubsystem::GetToolConfig(FName RowName) const
{
	if (!ToolConfigTable) return nullptr;
	return ToolConfigTable->FindRow<FToolConfigRow>(RowName, TEXT("GetToolConfig"));
}

const FDroneConfigRow* UGameDataSubsystem::GetDroneConfig(FName RowName) const
{
	if (!DroneConfigTable) return nullptr;
	return DroneConfigTable->FindRow<FDroneConfigRow>(RowName, TEXT("GetDroneConfig"));
}
