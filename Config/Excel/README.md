# LetsDeepRock 配置表使用说明

## 目录结构

```
Config/Excel/
├── README.md               ← 本文件
├── DT_EnemyConfig.csv      ← 敌人属性配置表
├── DT_SpawnerConfig.csv    ← 生成器/波次配置表
└── DT_GameModeConfig.csv   ← 游戏模式全局参数配置表
```

## 快速开始

### 方法一：直接在 UE 编辑器中编辑（推荐新手）

1. 打开 UE 编辑器
2. 在 Content Browser 中右键 → Miscellaneous → Data Table
3. 选择对应的 Row Structure：
   - `FEnemyConfigRow` → 创建为 `Content/Data/DT_EnemyConfig`
   - `FSpawnerConfigRow` → 创建为 `Content/Data/DT_SpawnerConfig`
   - `FGameModeConfigRow` → 创建为 `Content/Data/DT_GameModeConfig`
4. 双击打开 DataTable，手动添加行和填写数值

### 方法二：从 CSV 导入（推荐策划批量编辑）

1. 用 Excel 或文本编辑器修改本目录下的 `.csv` 文件
2. 在 UE 编辑器中：
   - 如果 DataTable 已存在：右键 DataTable → Reimport
   - 如果是新建：右键 Content Browser → Import → 选择 .csv 文件 → 选择对应 Row Structure
3. **注意**：CSV 必须是 UTF-8 编码

### 方法三：在 Excel 中编辑后另存为 CSV

1. 打开 `.csv` 文件（或创建同结构的 `.xlsx`）
2. 编辑数据
3. 另存为 → CSV UTF-8（逗号分隔）
4. 在 UE 编辑器中 Reimport

## CSV 格式规范

- 第一行为字段名（必须与 C++ 结构体字段名完全匹配）
- 第一列为 RowName（行标识符，代码中通过此名称查询）
- 编码：UTF-8
- 分隔符：逗号
- 字符串包含逗号时需用双引号包裹

## 各表字段说明

### DT_EnemyConfig（敌人属性）

| 字段 | 类型 | 说明 | 默认值 |
|------|------|------|--------|
| RowName | FName | 行标识符，蓝图中配置此名称 | - |
| DisplayName | FText | 敌人显示名称 | - |
| MaxHealth | float | 最大生命值 | 30 |
| MoveSpeed | float | 移动速度 | 400 |
| AttackDamage | float | 攻击伤害 | 10 |
| ScoreValue | int32 | 击杀奖励分数 | 1 |
| PickupSpawnChance | int32 | 掉落概率(0-100) | 10 |
| DeathLingerTime | float | 死亡延迟销毁(秒) | 2.0 |

### DT_SpawnerConfig（生成器参数）

| 字段 | 类型 | 说明 | 默认值 |
|------|------|------|--------|
| SpawnGroupDelay | float | 生成组间隔(秒) | 5.0 |
| SpawnGroupSize | int32 | 每组生成数量 | 3 |
| MinSpawnDelay | float | 个体最小间隔(秒) | 0.33 |
| MaxSpawnDelay | float | 个体最大间隔(秒) | 0.66 |
| SpawnRadius | float | 生成半径(cm) | 600 |
| NPCCap | int32 | 场上NPC上限 | 20 |

### DT_GameModeConfig（游戏模式参数）

| 字段 | 类型 | 说明 | 默认值 |
|------|------|------|--------|
| ComboIncrementMax | int32 | 连击递增阈值 | 5 |
| ComboCap | int32 | 最大连击倍率 | 4 |
| ComboCooldown | float | 连击冷却(秒) | 3.0 |
| NPCCap | int32 | NPC上限 | 20 |

## 蓝图中如何使用

在 NPC / Spawner / GameMode 蓝图的 Details 面板中，找到 **DataTable** 分类，
填入对应的 RowName（如 `BasicGrunt`、`Default`）。

- 留空 = 使用 C++ 默认值（向后兼容）
- 填入不存在的行名 = 使用默认值 + 控制台 Warning

## 后续扩展

需要新增字段时：
1. 在 `Source/LetsDeepRock/Data/DataTableRows.h` 中给对应结构体添加 UPROPERTY
2. 重新编译
3. DataTable 中自动出现新列
4. 更新本目录下的 CSV 文件添加新列
