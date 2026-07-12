# Unreal MCP Server

通过 MCP 协议让 AI Agent 直接操作 Unreal Engine 5.8 编辑器。

## 架构

```
AI Agent (OpenCode) → stdio → MCP Server (Python) → UE Editor
                                    │
                                    ├── Python Remote Exec (UDP+TCP) ← 主通道
                                    └── Remote Control API (HTTP)    ← 补充通道
```

## 工具清单 (19 个)

### Tier 1 — 资产与蓝图核心操作
| 工具 | 功能 |
|------|------|
| `create_data_asset` | 创建 DataAsset（如 WeaponDataAsset） |
| `create_data_table` | 创建 DataTable 并填充行数据 |
| `search_assets` | 搜索资产（按类/路径/关键词） |
| `get_asset_properties` | 读取资产属性值 |
| `create_blueprint` | 创建 Blueprint（指定父类） |
| `set_blueprint_defaults` | 设置 Blueprint 的 Class Defaults |
| `add_component_to_blueprint` | 给 Blueprint 添加组件 |
| `compile_blueprint` | 编译 Blueprint |

### Tier 2 — GAS 与输入系统
| 工具 | 功能 |
|------|------|
| `create_gameplay_effect` | 创建 GameplayEffect（Duration/Modifiers/Execution） |
| `create_gameplay_ability` | 创建 GameplayAbility Blueprint |
| `create_input_action` | 创建 Enhanced Input Action |
| `add_imc_mapping` | 在 IMC 中添加按键绑定 |
| `remove_imc_mapping` | 移除 IMC 中的绑定 |

### Tier 3 — 关卡与通用工具
| 工具 | 功能 |
|------|------|
| `spawn_actor_in_level` | 在关卡中放置 Actor |
| `build_navigation` | 构建 NavMesh |
| `create_material` | 创建简单材质 |
| `create_blackboard` | 创建 AI Blackboard + 添加键 |
| `execute_python` | 执行任意 Python 代码（万能逃生口） |
| `execute_console_command` | 执行 UE 控制台命令 |
| `save_asset` | 保存资产 |
| `get_editor_status` | 获取编辑器状态 |
| `list_assets_in_path` | 列出路径下资产 |

---

## 安装

### 1. 安装 MCP Server

```powershell
cd Tools/UnrealMCP
pip install -e .
```

或使用 uv：

```powershell
cd Tools/UnrealMCP
uv pip install -e .
```

### 2. 配置 UE 编辑器

在 UE 5.8 编辑器中启用以下插件：

#### 必须启用：Python Remote Execution（主通道）

1. **Edit → Plugins** → 搜索 "Python" → 启用：
   - ✅ `Python Editor Script Plugin`
   - ✅ `Python Remote Execution`（5.8 中可能叫 "Python Scripting"）

2. **Edit → Project Settings → Plugins → Python Remote Execution**：
   - ✅ Enable Remote Execution = **True**
   - Multicast Group Endpoint = `239.0.0.1:6766`
   - Multicast Bind Address = **`0.0.0.0`**（重要！默认 127.0.0.1 可能导致外部工具连不上）
   - Receive Buffer Size = 65536（默认即可）

3. 重启编辑器使插件生效

#### 可选启用：Remote Control API（补充通道）

1. **Edit → Plugins** → 搜索 "Remote" → 启用：
   - ✅ `Remote Control API`

2. **Edit → Project Settings → Plugins → Remote Control**：
   - Enable = True
   - HTTP Port = 30010（默认）
   - Bind to `127.0.0.1`

3. 如果是 UE 5.8，还需在 `Config/DefaultRemoteControl.ini` 中添加：
   ```ini
   [/Script/RemoteControl.RemoteControlSettings]
   bAllowAnyRemoteFunctionCall=True
   ```

### 3. 配置 OpenCode

在项目的 `opencode.json` 中添加 MCP Server：

```json
{
  "mcp": {
    "unreal": {
      "command": "python",
      "args": ["-m", "unreal_mcp.server"],
      "env": {
        "PYTHONPATH": "./Tools/UnrealMCP/src"
      }
    }
  }
}
```

或者如果已 `pip install -e .`：

```json
{
  "mcp": {
    "unreal": {
      "command": "unreal-mcp"
    }
  }
}
```

---

## 使用示例

### 通过 AI Agent 自动创建 DataAsset

Agent 调用 `create_data_asset` 工具：
```
asset_name: "DA_Shotgun"
package_path: "/Game/Variant_Weapon/Weapons"
parent_class: "WeaponDataAsset"
properties: {"BaseDamage": 20.0, "FireInterval": 0.8}
```

### 创建 GameplayEffect

```
asset_name: "GE_MeleeCooldown"
package_path: "/Game/Variant_Weapon/GAS"
duration_policy: "HasDuration"
duration_magnitude: 0.5
```

### 执行任意代码（万能逃生口）

```
code: "import unreal; print(unreal.SystemLibrary.get_engine_version())"
```

---

## 故障排查

| 问题 | 原因 | 解决 |
|------|------|------|
| 连接失败（Python Remote Exec） | 插件未启用或 Multicast Bind Address 设置不对 | 确认启用插件，Bind Address 改为 `0.0.0.0` |
| 连接失败（Remote Control API） | 插件未启用或端口被占 | 确认插件启用，检查端口 30010 |
| 工具执行无响应 | UE 编辑器未启动 | 先启动编辑器再启动 MCP Server |
| 资产创建后看不到 | Content Browser 未刷新 | 右键文件夹 → Refresh |
| Blueprint 编译失败 | 属性设置不正确 | 检查返回的错误信息 |

---

## 开发

```
Tools/UnrealMCP/
├── pyproject.toml              # 项目配置
├── README.md                   # 本文件
└── src/
    └── unreal_mcp/
        ├── __init__.py
        ├── server.py           # MCP Server 主入口
        ├── connection/         # UE 连接层
        │   ├── __init__.py
        │   ├── python_remote.py   # Python Remote Exec 客户端
        │   └── remote_control.py  # Remote Control API 客户端
        └── tools/              # MCP 工具集
            ├── __init__.py
            ├── asset_tools.py     # 资产操作
            ├── blueprint_tools.py # 蓝图操作
            ├── gameplay_tools.py  # GAS 操作
            ├── input_tools.py     # 输入系统
            ├── level_tools.py     # 关卡操作
            └── utility_tools.py   # 通用工具
```
