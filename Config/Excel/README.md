# LetsDeepRock 配置表使用说明

## 工作流概览

```
策划编辑 .xlsx (Config/Excel/)
        │
        ▼  Tools/ExcelExporter/ 自动导出
.csv 文件 (Config/Excel/)
        │
        ▼  UE 编辑器 Reimport
Content/Data/DT_*.uasset
        │
        ▼  运行时
GameDataSubsystem 加载 → 按 RowName 查询
```

**版本管理**：xlsx 和 csv 都通过 Git 管理。

## 目录结构

```
Config/Excel/
├── README.md                       ← 本文件
├── DT_EnemyConfig.xlsx             ← 策划编辑源（敌人属性）
├── DT_EnemyConfig.csv              ← 自动导出（UE 导入用）
├── DT_CharacterAttributes.xlsx     ← 策划编辑源（角色/职业属性）
├── DT_CharacterAttributes.csv      ← 自动导出
├── DT_DamageTypeModifier.xlsx      ← 策划编辑源（伤害类型克制表）
├── DT_DamageTypeModifier.csv       ← 自动导出
├── DT_SpawnerConfig.csv            ← 旧格式（待迁移到 xlsx）
└── DT_GameModeConfig.csv           ← 旧格式（待迁移到 xlsx）
```

## 快速开始

### 策划日常改表流程

1. 打开 `Config/Excel/DT_XxxConfig.xlsx`，编辑数据
2. 保存 Excel
3. 运行导出工具（三选一）：
   - **GUI**：双击 `Tools/ExcelExporter/excel_exporter_gui.pyw`，点「全部导出」
   - **命令行**：`python Tools/ExcelExporter/excel_exporter.py Config/Excel/`
   - **一键脚本**：双击 `Tools/ExcelExporter/export_all.bat`
4. CSV 自动覆盖更新
5. UE 编辑器中 Reimport DataTable（右键 → Reimport）
6. `git add` + `commit`（xlsx 和 csv 一起提交）

### 首次安装依赖

```bash
pip install openpyxl
```

## Excel 表头格式规范

每个 xlsx 文件需遵循 **3 行表头** 约定：

| 行 | 用途 | 示例 |
|---|---|---|
| 1 | 元数据（`#` 开头） | `#TABLE=DT_EnemyConfig` `#STRUCT=FEnemyConfigRow` |
| 2 | UE 数据类型 | `FName` / `FText` / `float` / `int32` / `bool` / `FGameplayTag` |
| 3 | 字段名（= C++ UPROPERTY 名） | `DisplayName` / `MaxHealth` / `MoveSpeed` |
| 4+ | 数据行 | 第一列为 RowName |

### 支持的类型

| 类型标记 | 说明 | CSV 输出格式 |
|---------|------|-------------|
| `int32` | 整数 | `42` |
| `float` | 浮点 | `3.14` |
| `bool` | 布尔 | `True` / `False` |
| `FText` | 文本（支持中文） | `"基础虫族"` |
| `FString` | 字符串 | `"hello"` |
| `FName` | 名称标识 | `BasicGrunt` |
| `FGameplayTag` | GameplayTag | `(TagName="ArmorType.Heavy")` |
| `FVector` | 三维向量 | `(X=1.0,Y=2.0,Z=3.0)` |

### 注释规则

- 以 `#` 开头的**行**会被跳过（不导出）
- 以 `#` 开头的**列**会被跳过
- 空行会被跳过

## 各配置表字段说明

### DT_EnemyConfig（敌人属性）

| 字段 | 类型 | 说明 | 默认值 |
|------|------|------|--------|
| RowName | FName | 行标识符 | - |
| DisplayName | FText | 显示名称 | - |
| MaxHealth | float | 最大生命值 | 30 |
| MoveSpeed | float | 移动速度 | 400 |
| AttackDamage | float | 攻击伤害 | 10 |
| ScoreValue | int32 | 击杀奖励分数 | 1 |
| PickupSpawnChance | int32 | 掉落概率(0-100) | 10 |
| DeathLingerTime | float | 死亡延迟销毁(秒) | 2.0 |
| Armor | float | 护甲值 | 0 |
| MaxShield | float | 最大护盾 | 0 |
| ResistPhysical~Electric | float | 6种抗性(0~1) | 0 |
| ArmorTypeTag | FGameplayTag | 护甲类型 | - |

### DT_CharacterAttributes（角色/职业属性）

30 个属性字段，覆盖生存/战斗/移动/抗性/资源五大类。详见 xlsx 文件内表头。

### DT_DamageTypeModifier（伤害类型克制）

| 字段 | 类型 | 说明 |
|------|------|------|
| RowName | FName | 伤害类型 Tag 名（如 `Damage.Type.Fire`） |
| VsUnarmored | float | 对无甲修正系数 |
| VsLight | float | 对轻甲修正系数 |
| VsHeavy | float | 对重甲修正系数 |
| VsOrganic | float | 对有机体修正系数 |

## 新增配置表

1. 在 `Source/LetsDeepRock/Data/DataTableRows.h` 中新增 `USTRUCT`
2. 重新编译
3. 在 `Config/Excel/` 下创建对应的 `.xlsx`（按表头格式规范）
4. 运行导出工具生成 `.csv`
5. UE 编辑器中 Import CSV → 选择对应 Row Structure
6. 将 `.xlsx` 和 `.csv` 一起提交 Git

## 注意事项

- **xlsx 是源文件**，csv 是自动生成的产物，**不要手动编辑 csv**
- xlsx 和 csv 都走 **Git** 管理（不是 P4）
- UE 的 `.uasset` DataTable 走 **P4** 管理
- CSV 编码为 UTF-8 with BOM（UE 偏好）
- 字段名必须与 C++ `UPROPERTY` 名称**完全一致**（区分大小写）
