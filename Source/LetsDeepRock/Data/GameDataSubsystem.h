// 游戏数据配置子系统
// 提供对 DataTable 的集中访问，避免每个 Actor 各自 LoadObject

#pragma once

#include "CoreMinimal.h"
#include "Subsystems/GameInstanceSubsystem.h"
#include "DataTableRows.h"
#include "GameDataSubsystem.generated.h"

/**
 * 游戏数据配置子系统
 *
 * 作为 GameInstanceSubsystem 自动跟随 GameInstance 生命周期。
 * 集中管理所有 DataTable 的加载和访问，避免散落的 LoadObject 调用。
 *
 * 用法:
 *   UGameDataSubsystem* DataSys = GetGameInstance()->GetSubsystem<UGameDataSubsystem>();
 *   const FEnemyConfigRow* Row = DataSys->GetEnemyConfig(FName("BasicGrunt"));
 */
UCLASS()
class UGameDataSubsystem : public UGameInstanceSubsystem
{
	GENERATED_BODY()

public:
	virtual void Initialize(FSubsystemCollectionBase& Collection) override;

	// ---- 敌人配置 ----

	/** 获取敌人配置行，返回 nullptr 表示未找到 */
	const FEnemyConfigRow* GetEnemyConfig(FName RowName) const;

	// ---- 生成器配置 ----

	/** 获取生成器配置行 */
	const FSpawnerConfigRow* GetSpawnerConfig(FName RowName) const;

	// ---- 游戏模式配置 ----

	/** 获取游戏模式配置行 */
	const FGameModeConfigRow* GetGameModeConfig(FName RowName) const;

	// ---- 角色属性配置 ----

	/** 获取角色属性配置行 */
	const FCharacterAttributeRow* GetCharacterAttributes(FName RowName) const;

	// ---- 职业与装备配置 ----

	const FProfessionConfigRow* GetProfessionConfig(FName RowName) const;
	const FWeaponConfigRow* GetWeaponConfig(FName RowName) const;
	const FWeaponFireModeConfigRow* GetWeaponFireModeConfig(FName RowName) const;
	const FGrenadeConfigRow* GetGrenadeConfig(FName RowName) const;
	const FStatusEffectConfigRow* GetStatusEffectConfig(FName RowName) const;
	const FToolConfigRow* GetToolConfig(FName RowName) const;
	const FDroneConfigRow* GetDroneConfig(FName RowName) const;

protected:
	/** 敌人配置表资产路径（Content/Data/DT_EnemyConfig） */
	UPROPERTY()
	TObjectPtr<UDataTable> EnemyConfigTable;

	/** 生成器配置表资产路径 */
	UPROPERTY()
	TObjectPtr<UDataTable> SpawnerConfigTable;

	/** 游戏模式配置表资产路径 */
	UPROPERTY()
	TObjectPtr<UDataTable> GameModeConfigTable;

	/** 角色属性配置表资产 */
	UPROPERTY()
	TObjectPtr<UDataTable> CharacterAttributeTable;

	/** DT_ProfessionConfig */
	UPROPERTY()
	TObjectPtr<UDataTable> ProfessionConfigTable;

	/** DT_WeaponConfig */
	UPROPERTY()
	TObjectPtr<UDataTable> WeaponConfigTable;

	/** DT_WeaponFireModeConfig */
	UPROPERTY()
	TObjectPtr<UDataTable> WeaponFireModeConfigTable;

	/** DT_GrenadeConfig */
	UPROPERTY()
	TObjectPtr<UDataTable> GrenadeConfigTable;

	/** DT_StatusEffectConfig */
	UPROPERTY()
	TObjectPtr<UDataTable> StatusEffectConfigTable;

	/** DT_ToolConfig */
	UPROPERTY()
	TObjectPtr<UDataTable> ToolConfigTable;

	/** DT_DroneConfig */
	UPROPERTY()
	TObjectPtr<UDataTable> DroneConfigTable;
};
