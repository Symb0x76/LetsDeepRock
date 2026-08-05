---
name: letsdeeprock-config-table-pipeline
description: 该技能在用户要求为 LetsDeepRock 新增、修改、导出或导入配置表，或排查 USTRUCT、xlsx、CSV、DataTable 与 GameDataSubsystem 配置链路时使用；它执行项目配置表全链路，同时保留 Excel、Unreal MCP、Git 与 P4 的明确授权边界。
---

# LetsDeepRock 配置表全链路

## 适用范围

当当前任务明确涉及 LetsDeepRock 的配置表时使用本技能，包括：

- 新增或修改配置行结构、`.xlsx`、`.csv`、DataTable；
- 接入或调整 `UGameDataSubsystem` 的表加载与查询；
- 运行 `ExcelExporter`，或定位表头、字段、导出、导入不一致；
- 用户明确要求把 CSV 导入 UE DataTable。

普通 C++ 功能开发若不涉及配置表，不触发本技能。遵守项目快速原型原则：只完成当前表和当前调用链，不预建通用配置框架、迁移系统或生产级校验层。

## 权威路径与格式

项目根目录为 `D:\testfiles\UnrealDeepRock\LetsDeepRock`。以项目内现状为准，关键位置如下：

- C++ 行结构与数据访问：`Source/LetsDeepRock/`，重点检查 `Source/LetsDeepRock/Data/GameDataSubsystem.h` 和 `Source/LetsDeepRock/Data/GameDataSubsystem.cpp`；
- 原始表与导出结果：`Config/Excel/DT_*.xlsx` 和 `Config/Excel/DT_*.csv`；
- 导出工具：`Tools/ExcelExporter/excel_exporter.py`；
- UE DataTable：`Content/Data/DT_*.uasset`，由 P4 管理，不进入 Git。

项目配置表使用四行表头：

1. 第 1 行是元数据，例如 `#TABLE=DT_EnemyConfig`、`#STRUCT=FEnemyConfigRow`；
2. 第 2 行是 UE 类型，例如 `FName`、`FText`、`float`、`int32`、`bool`、`FGameplayTag`；
3. 第 3 行是字段名，必须对应 C++ `UPROPERTY`；
4. 第 4 行是中文说明，导出时跳过；
5. 第 5 行起是数据，第一列为 `RowName`。

字段名行或类型行以 `#` 开头的列属于备注列，不导出；第一列后的第二列推荐使用 `#备注`。

## 授权边界

- 默认只做文件和代码层面的工作及静态验证，不连接 UE，不执行远程导入，不启动 UE，不运行 PIE。
- 只有用户在当前任务明确授权“使用 Unreal MCP 远程配置/导入”时，才可连接用户已启动的 UE 编辑器并导入 DataTable。历史授权、普通“实现功能”或仅要求生成 CSV 均不构成授权。
- 修改 Excel 必须先读取定位、执行 `dry-run`、向用户展示预演差异并等待明确确认；确认后才能真正写入。
- `git add`、commit、push、分支操作以及 P4 的 `sync`、`reconcile`、`revert`、submit 等副作用均需用户明确授权。
- `Source/`、`Config/`、`.uproject` 属于 Git；`Content/` 属于 P4。不得把 `Content/` 加入 Git，也不得把代码和配置提交到 P4。
- 不覆盖、清理或回滚用户已有改动。发现同一文件有无关改动时保留并只做目标范围内编辑。

## 执行流程

### 1. 先检查现有链路

在创建任何内容前，按当前任务范围检查：

1. 使用 `rg` 查找目标 `FTableRowBase` USTRUCT、字段名和引用点；
2. 检查 `UGameDataSubsystem` 是否已有对应 DataTable 属性、硬编码加载路径和查询函数；
3. 检查 `Config/Excel/` 中同名 `.xlsx`、`.csv` 及相邻表的命名和字段惯例；
4. 检查目标文件的 Git 状态；若任务获准写 UE 资产，再检查对应 `Content/Data/` 的 P4 状态；
5. 复用已有结构和路径，不创建重复的 Struct、表或加载入口。

### 2. 修改 C++

- 行结构继承 `FTableRowBase`，字段名与第 3 行表头精确一致；
- 只加入当前数据所需的 `USTRUCT`、`UPROPERTY`、加载和查询逻辑；
- 若新增表，沿用 `UGameDataSubsystem` 当前的加载方式和 `/Game/Data/DT_*` 路径；
- 不为单表需求引入注册中心、反射封装、版本迁移或额外配置层；
- 保持项目源码 CRLF 行尾。

### 3. 安全修改 xlsx

`.xlsx`/`.xlsm` 必须使用 `excel-com` MCP，不得把二进制文件当文本处理。项目 `Config\Excel` 已加入 allowlist，可直接读取目标表，不需要复制到 `D:\workspace`，读取和校验也不需要额外用户授权。按以下顺序执行：

1. 用 `excel_com_validate` 确认可读，再用 `excel_com_read` 读取目标表头、数据区域和当前值；
2. 用 `excel_com_patch(..., dry_run=true)` 预演，检查逐操作 diff、`changed_cells`、`unchanged_cells` 及目标单元格；
3. 向用户展示预演差异，暂停并等待明确确认；
4. 确认后使用相同 operations 调用 `excel_com_patch(..., dry_run=false)` 正式写入；
5. 正式写入后用 `excel_com_read` 回读目标范围，核对元数据、类型、字段名、说明、RowName 和数据值；
6. 新建工作簿使用 `excel_com_create`，且只能创建 `.xlsx`；新建后同样使用 `excel_com_read` 回读。

当前 patch/create 只支持 `set_value` 和 `set_range_values`，批量矩形区域优先使用 `set_range_values`。公式只能读取，不能写入；样式、图表、合并单元格等能力也未支持。遇到这些需求时必须停下并明确说明，不允许自动回退到 `openpyxl` 或其他后端。只改当前任务指定的表和单元格。

### 4. 导出 CSV

从项目根目录运行：

```powershell
python Tools/ExcelExporter/excel_exporter.py Config/Excel/ --output Config/Excel/
```

若只需单表且工具支持单文件输入，优先只导出目标表；否则批量导出后只保留工具对目标范围产生的预期变化，不擅自改动其他 CSV。检查导出的列名、RowName、数据行数和关键值，并保持文本文件 CRLF。

### 5. 导入 UE DataTable

仅在用户当前任务明确授权 Unreal MCP 远程导入后执行：

1. 确认用户已启动正确的 LetsDeepRock UE 5.8 编辑器，且 Python Remote Execution 已开启；
2. 连接前再次核对目标 CSV、Row Struct、资产路径和现有资产；
3. 优先 Reimport 已有 `Content/Data/DT_*.uasset`；仅目标不存在时创建；
4. 只保存明确指定的 DataTable，不执行全量保存，不启动 PIE；
5. 导入后回读并核对 `RowStruct`、行数、全部或目标 `RowName` 及关键字段；
6. 将结果记为“静态确认/编辑器配置确认”，不能因导入成功声称玩法已实机确认。

若未获得远程授权，到 CSV 与静态检查为止，并向用户提供手动 Reimport/导入步骤。

## 静态验证与交付

至少完成以下静态核对：

- USTRUCT 字段、UE 类型、xlsx 第 2/3 行与 CSV 列名一致；
- `#TABLE`、`#STRUCT`、文件名、DataTable 资产名及 `UGameDataSubsystem` 路径互相对应；
- xlsx 与 CSV 的数据行数、RowName 唯一性和本次关键值一致；
- 新增 C++ 声明与定义匹配，引用点使用正确结构；
- `git diff --check` 无格式错误，改动文本为 CRLF；
- Git 改动中不包含 `Content/`，P4 范围中不包含 `Source/`、`Config/` 或 `.uproject`。

最终说明已完成的文件层改动、未执行的授权操作，并明确“改动已完成，请你实机验证”。只有用户反馈或明确运行证据才能标记为“已实机确认”。
