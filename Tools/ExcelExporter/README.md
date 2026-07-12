# LetsDeepRock Excel 导出工具

将 UE5 策划配置表 Excel (.xlsx) 导出为 UE DataTable 兼容的 CSV 格式。

## 安装依赖

```bash
pip install -r requirements.txt
```

只需要 `openpyxl` 一个第三方库，tkinter 为 Python 自带模块。

## GUI 使用方法

双击 `excel_exporter_gui.pyw` 即可启动图形界面：

1. **输入目录**：选择存放 .xlsx 文件的目录（默认 `../../Config/Excel/`）
2. **输出目录**：选择 CSV 导出目标目录（默认同输入目录）
3. **文件列表**：勾选需要导出的文件（默认全选）
4. 点击 **「全部导出」** 导出所有文件，或 **「导出选中」** 只导出勾选的文件
5. 查看下方日志了解导出详情

## CLI 使用方法

### 单文件导出

```bash
python excel_exporter.py DT_EnemyConfig.xlsx
```

### 目录批量导出

```bash
python excel_exporter.py "Config/Excel" --output "Config/Excel"
```

### 一键批量导出

双击 `export_all.bat`，自动导出 `Config/Excel` 目录下所有 .xlsx。

### 参数说明

| 参数 | 说明 |
|------|------|
| `target` | 必填，单个 .xlsx 文件 或 目录路径 |
| `--output` / `-o` | 输出目录（默认同输入目录） |
| `--no-validate` | 跳过类型校验 |

## Excel 表头格式

表格需要 3 行表头（或 2 行，无元数据时）：

| 行号 | 用途 | 示例 |
|------|------|------|
| 1 | 配置元数据（可选，以 `#` 开头） | `#TABLE=DT_EnemyConfig` `#STRUCT=FEnemyConfigRow` |
| 2 | UE 类型 | `FName` `int32` `float` `bool` `FText` `FGameplayTag` |
| 3 | 字段名（与 C++ UPROPERTY 对应） | `RowName` `DisplayName` `MaxHealth` |
| 4+ | 数据行 | 按字段填入数值 |

**注意**：
- 如果第 1 行第 1 列不以 `#` 开头，则视为无元数据行，第 1 行 = 类型行，第 2 行 = 字段名行
- 以 `#` 开头的行或列会被跳过
- 空行自动跳过

## 支持的类型

| Excel 类型标记 | CSV 输出格式 | 示例 |
|---------------|-------------|------|
| `FName` | 直接输出 | `BasicGrunt` |
| `int32` | 取整 | `100` |
| `float` | 保留小数 | `1.5` |
| `bool` | True/False | `True` |
| `FText` | 双引号包裹 | `"基础虫族"` |
| `FString` | 双引号包裹 | `"Hello"` |
| `FGameplayTag` | `(TagName="...")` | `(TagName="Enemy.Grunt")` |
| `FGameplayTagContainer` | `(GameplayTags=((...)))` | `(GameplayTags=((TagName="A"),(TagName="B")))` |
| `FVector` | `(X=,Y=,Z=)` | `(X=100,Y=200,Z=50)` |
| `FVector2D` | `(X=,Y=)` | `(X=100,Y=200)` |
| `FRotator` | `(P=,Y=,R=)` | `(P=0,Y=90,R=0)` |

对于 FVector / FVector2D / FRotator，Excel 中可以直接填写逗号分隔的数字（如 `100,200,300`），工具会自动转换格式。

## CSV 输出格式

导出后的 CSV 符合 UE DataTable 格式要求：
- 第一行为字段名，第一列标题为 `---`（UE 规范占位符）
- 数据从第二行开始，第一列为 RowName
- 编码：UTF-8 with BOM

```csv
---,DisplayName,MaxHealth,MoveSpeed
BasicGrunt,"基础虫族",100,400
EliteGrunt,"精英虫族",300,500
```

## 注意事项

- 只支持 `.xlsx` 格式（Excel 2007+），不支持 `.xls`
- Excel 临时文件（`~$` 开头）会被自动跳过
- 导出前请关闭正在编辑的 Excel 文件，避免文件锁定
- 类型校验仅做格式检查，不验证业务逻辑（如血量不能为负等）
- 建议在提交 CSV 到版本控制前，先在 UE 中验证 DataTable 导入是否正常
