#include "GameDataSubsystem.h"
#include "Engine/DataTable.h"

void UGameDataSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);

	// 加载 DataTable 资产
	// 路径格式: /Game/Data/资产名.资产名
	// 如果资产不存在（尚未在编辑器中创建），返回 nullptr 不会崩溃
	EnemyConfigTable = LoadObject<UDataTable>(nullptr,
		TEXT("/Game/Data/DT_EnemyConfig.DT_EnemyConfig"));
	SpawnerConfigTable = LoadObject<UDataTable>(nullptr,
		TEXT("/Game/Data/DT_SpawnerConfig.DT_SpawnerConfig"));
	GameModeConfigTable = LoadObject<UDataTable>(nullptr,
		TEXT("/Game/Data/DT_GameModeConfig.DT_GameModeConfig"));

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
