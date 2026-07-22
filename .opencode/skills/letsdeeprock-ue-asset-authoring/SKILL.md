---
name: letsdeeprock-ue-asset-authoring
description: 该技能仅在用户当前任务明确要求使用 Unreal MCP、在引擎中配置，或创建、修改、导入 UE 资产时使用；它约束 LetsDeepRock 的远程资产编写、保存与回读验证，避免普通功能实现自动获得资产写入权限。
---

# LetsDeepRock UE 资产编写

## 严格触发条件

仅当用户在当前任务明确表达以下意图之一时使用本技能：

- “使用 Unreal MCP”；
- “在引擎中配置”；
- “创建/修改/导入 UE 资产、蓝图、组件、GameplayEffect、DataTable、Input Mapping Context”；
- 其他含义同等明确的 UE 编辑器内写操作。

普通“实现功能”“修复代码”“增加配置”不授予连接编辑器或写资产权限；仅提到某个资产路径、要求提供手动步骤或过去曾授权，也不触发远程写入。没有当前任务的明确授权时，只检查代码/配置并给出人工操作步骤。

本技能服务于 `D:\testfiles\UnrealDeepRock\LetsDeepRock` 的 UE 5.8 快速原型。只创建当前功能需要的资产和配置，不扩展为通用资产流水线、批处理框架或生产级恢复系统。

## 不可逾越的边界

- 不自动启动、重启或关闭 UE；连接用户已经启动的正确编辑器实例。
- 不自动运行 PIE、Standalone、客户端或服务端。
- 不执行全量保存，不使用会保存全部脏包的操作；只保存用户指定且本次实际修改的资产。
- 不擅自修改关卡、World Settings、GameMode、Input 或其他关联资产；只有明确属于当前需求时才处理。
- 不自动执行 P4 `sync`、`reconcile`、submit、revert，也不执行 Git 副作用；这些操作需要单独明确授权。
- `Content/` 资产只由 P4 管理，绝不加入 Git。
- 不把工具调用成功等同于玩法可用或实机验证通过。

## 连接前置检查

在任何写调用之前：

1. 确认当前任务具有明确 Unreal MCP/引擎内写入授权，并复述目标资产和允许的操作范围；
2. 确认用户已经启动 LetsDeepRock 的 UE 5.8 编辑器，Python Remote Execution/对应 MCP 能力已开启；
3. 核对连接到的项目名、`.uproject` 路径和编辑器实例，避免写入其他工程；
4. 查看 Git/P4 状态和目标资产是否已存在、是否已打开、是否有用户未提交改动；发现冲突或所有权不明时停止写入并报告；
5. 确认目标父类、包路径、资产名、依赖类和预期最终配置；
6. 先使用只读能力查询目标，不通过创建操作试探环境。

如果编辑器未运行、连接能力未开启或项目身份无法确认，停止并请用户处理前置条件，不自行启动 UE。

## 执行原则

### 先查后建

- 先查询完整对象路径和资产类型；存在时按需求修改，不重复创建同名资产；
- 创建前检查父类、现有组件、变量、Graph、RowName 或 Input mapping，防止重复条目；
- 对可能重名的对象使用包路径而不是只按短名称判断；
- 修改前记录本次相关的原值，以便准确报告差异，但不擅自实现通用回滚系统。

### 最小工具优先

优先使用 Unreal MCP 提供的专用资产/蓝图/DataTable/Input 工具完成单一操作，并立即回读。只有专用工具缺失、无法表达所需属性或已知行为不可靠时，才使用 `execute_python` 兜底。

使用 `execute_python` 时：

- 脚本只处理明确指定的对象路径，不扫描或批量改写整个 `/Game`；
- 先加载并检查类型，再修改必要属性；
- 不调用全量保存、PIE、Editor 重启或进程控制 API；
- 输出目标对象、关键修改和失败原因，避免输出无关的大量对象数据；
- API 不确定时先通过只读反射或查询确认，不连续猜测写入。

### 指定保存与逐项回读

每完成一个逻辑单元：

1. 编译需要编译的 Blueprint；
2. 只保存本次明确修改的资产包；
3. 重新加载或调用只读接口，逐项核对资产类型、父类、组件、属性、引用、行或 mapping；
4. 发现回读不符时停止后续依赖操作，报告实际值，不把工具返回的成功字段当作最终证据；
5. 完成后列出实际保存的资产路径，不保存其他脏资产。

## UE 5.8 当前可靠模式

### 创建 Blueprint

创建 Blueprint 时使用 `BlueprintFactory`，并通过 `set_editor_property` 设置父类，再创建资产。概念模式如下：

```python
factory = unreal.BlueprintFactory()
factory.set_editor_property("parent_class", parent_class)
blueprint = asset_tools.create_asset(asset_name, package_path, unreal.Blueprint, factory)
```

创建后回读 `ParentClass`/Generated Class，确认不是默认或错误父类。不要仅凭 `create_asset` 返回非空就继续。

### 添加 Blueprint 组件

使用 `SubobjectDataSubsystem` 获取 Blueprint 的 subobject 数据并添加组件。添加前按组件类和名称检查是否已经存在；添加后回读组件层级、组件类、名称及本次设置的关键属性，防止重复组件或挂到错误父节点。

完成结构或默认值修改后使用：

```python
unreal.BlueprintEditorLibrary.compile_blueprint(blueprint)
```

检查编译状态，再只保存该 Blueprint。

### 配置 GameplayEffect execution calculation

设置 GameplayEffect 的 execution calculation 时核对目标类是预期 `GameplayEffectExecutionCalculation` 子类。专用属性写入不接受类引用时，可在确认属性类型后对 `calculation_class` 使用 `import_text` 形式写入。写入后必须回读 execution definitions 和 `calculation_class` 的完整类路径，并确认没有重复 execution 条目。

### 填充 DataTable

可使用项目适合的 JSON 或 CSV 导入/填充方式，但在写入前先核对目标 DataTable 与 Row Struct。写入并保存后至少回读：

- `RowStruct` 的完整路径；
- 实际行数；
- 所有或本次目标 `RowName`；
- 本次修改行的关键字段值。

仅“导入命令成功”不足以证明 Row Struct 或数据正确。已有表优先 Reimport/定向更新，避免无意清空其他行。

### 配置 Input Mapping Context

添加 mapping 前按 Input Action、Key 和已有条目检查，避免每次调用都追加重复 mapping。写入后回读 mapping 数量及 Action/Key 对应关系。

当前工具对 IMC mapping 的 `modifiers`/`triggers` 配置不能视为已经生效：即使调用返回成功，也必须通过可靠的对象回读确认；无法回读确认时明确标记为“待验证”，并让用户在编辑器中手动核对或配置。不得宣称 modifiers/triggers 已完成。

## 验证状态与交付

“静态确认”只表示代码、对象结构、路径、属性回读、Blueprint 编译状态或 DataTable 内容在编辑器/API 层面一致。“已实机确认”必须来自用户执行 PIE/游戏后的反馈或明确运行证据。

交付时报告：

- 创建、修改并实际保存的资产完整路径；
- 每个资产逐项回读的关键结果；
- 未执行的操作，例如 PIE、全量保存、P4/Git 提交；
- 仍需用户手动检查的项目，尤其是 IMC modifiers/triggers 和玩法表现；
- 清晰说明“资产改动已完成，请你实机验证”，并给出最短可执行的 PIE 验证步骤。
