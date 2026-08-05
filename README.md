# LetsDeepRock

Unreal Engine 5.8 C++ 项目 — 受 Deep Rock Galactic 启发的多人合作射击游戏。

---

## 目录

- [环境要求](#环境要求)
- [版本控制概览](#版本控制概览)
- [首次拉取 & 环境搭建](#首次拉取--环境搭建)
- [编译 & 启动项目](#编译--启动项目)
- [Git 工作流](#git-工作流)
- [Perforce (P4) 工作流](#perforce-p4-工作流)
- [项目结构简介](#项目结构简介)
- [架构契约](#架构契约)
- [AI 编辑器自动化 (Unreal MCP)](#ai-编辑器自动化-unreal-mcp)
- [常见问题](#常见问题)

---

## 环境要求

| 工具 | 版本 / 说明 |
|------|-------------|
| **Unreal Engine** | 5.8（Epic Games Launcher 安装） |
| **Visual Studio** | 2022 17.x，需安装"使用 C++ 的游戏开发"工作负载 |
| **Git** | 2.40+（推荐安装 Git for Windows） |
| **Perforce** | P4V 2023.x+（Helix Visual Client），用于素材资源管理 |
| **Windows SDK** | 10.0.22621 或更高 |
| **.NET SDK** | 6.0+（UE 5.8 Build Tool 依赖） |

---

## 版本控制概览

本项目采用 **Git + Perforce 双版本控制**，职责严格分离：

| 管理对象 | 工具 | 远程仓库 |
|----------|------|----------|
| 代码 (`Source/`)、配置 (`Config/`)、项目定义 (`.uproject`) | **Git** | GitHub — `Mengluo972/LetsDeepRock` |
| 素材资源 (`Content/`) | **Perforce** | `ssl:symb0x76.top:1666`，Stream: `//deeprock/main` |

> **原则：Git 不提交 Content/，P4 不提交 Source/Config/。两者互不干扰。**

---

## 首次拉取 & 环境搭建

### 1. 克隆 Git 仓库（代码）

```powershell
git clone https://github.com/Mengluo972/LetsDeepRock.git
cd LetsDeepRock
```

> 克隆后你会得到 `Source/`、`Config/`、`LetsDeepRock.uproject` 等代码文件，但 **没有 Content/ 素材**。

### 2. 配置 Perforce（素材）

1. 安装 P4V (Helix Visual Client)
2. 创建 Workspace：
   - **Server**: `ssl:symb0x76.top:1666`
   - **User**: 向管理员申请账号
   - **Stream**: `//deeprock/main`
   - **Workspace Root**: 指向你刚克隆的 `LetsDeepRock/` 目录（与 Git 工作区重合）
3. 在项目根目录创建 `.p4config` 文件（**此文件已被 .gitignore 忽略，不会推到 GitHub**）：

```ini
P4PORT=ssl:symb0x76.top:1666
P4USER=你的P4用户名
P4PASSWD=你的P4密码
P4CHARSET=utf8
P4CLIENT=你的Workspace名称
P4IGNORE=.p4ignore
```

4. 设置全局环境变量 `P4CONFIG=.p4config`（使 p4 命令行在项目目录下自动加载配置）：

```powershell
[System.Environment]::SetEnvironmentVariable("P4CONFIG", ".p4config", "User")
```

5. 同步素材资源：

```powershell
p4 sync //deeprock/main/Content/...
```

> 同步完成后，`Content/` 目录将出现在项目中，UE 编辑器即可正常打开关卡和蓝图。

### 3. 生成项目文件

```powershell
# 方式一：使用 .uproject 右键菜单 → "Generate Visual Studio project files"
# 方式二：命令行
.\GenerateProjectFiles.bat
```

---

## 编译 & 启动项目

### 方式 A：通过 Visual Studio

1. 打开 `LetsDeepRock.sln`（由上一步生成）
2. 设置启动项目为 `LetsDeepRock`
3. 配置选择 `Development Editor | Win64`
4. `Ctrl+F5` 编译并启动 UE 编辑器

### 方式 B：通过 UE 编辑器

1. 在文件资源管理器中双击 `LetsDeepRock.uproject`
2. UE 编辑器会自动检测并编译 C++ 模块
3. 首次打开需要等待 Shader 编译完成

### 方式 C：命令行构建

```powershell
# 编译编辑器版本
.\BuildEditor.bat

# 重新完整构建
.\RebuildEditor.bat
```

---

## Git 工作流

采用 **Feature Branch + PR** 模式，`main` 分支受保护。

### 日常开发流程

```powershell
# 1. 从 main 创建功能分支
git checkout main
git pull origin main
git checkout -b feature/你的功能名

# 2. 开发 & 提交
git add Source/ Config/
git commit -m "feat: 描述你的改动"

# 3. 推送 & 开 PR
git push origin feature/你的功能名
# 在 GitHub 上创建 Pull Request → Review → Merge

# 4. 合入后清理
git checkout main
git pull origin main
git branch -d feature/你的功能名
```

### Commit 规范

格式：`type: description`（英文）

| type | 用途 |
|------|------|
| `feat` | 新功能 |
| `fix` | Bug 修复 |
| `refactor` | 重构（不改行为） |
| `docs` | 文档更新 |
| `chore` | 构建/配置等杂项 |

### Git 管理范围

**纳入版本控制的文件：**
- `Source/` — C++ 源码
- `Config/` — 项目配置 (.ini)
- `LetsDeepRock.uproject` — 项目定义
- `README.md` — 本文件
- `.gitignore`、`.gitattributes`

**忽略的文件（见 .gitignore）：**
- `Content/` — P4 管理
- `Binaries/`、`Intermediate/`、`DerivedDataCache/`、`Saved/` — UE 生成物
- `.sln`、`.vcxproj` 等 — IDE 工程文件（可再生成）
- `.p4config`、`.p4ignore`、`p4login.ps1` — P4 本地配置

---

## Perforce (P4) 工作流

### P4 管理范围

仅管理 `Content/` 目录下的所有素材资源（`.uasset`、`.umap` 等）。

### 日常素材操作

```powershell
# 修改已有素材：先 Checkout
p4 edit Content/Maps/MyLevel.umap
# 在 UE 编辑器中修改后...
p4 submit -d "更新 MyLevel 关卡"

# 新增素材：Mark for Add
p4 add Content/Textures/NewTexture.uasset
p4 submit -d "添加 NewTexture"

# 删除素材
p4 delete Content/OldAsset.uasset
p4 submit -d "移除废弃资产"
```

> **推荐使用 P4V 图形界面**进行素材操作，配合 UE 编辑器的 Source Control 集成更方便。

### .p4ignore 策略

`.p4ignore` 确保 P4 不会意外提交代码/配置/生成物。忽略项包括：

| 忽略类别 | 内容 |
|----------|------|
| Git 管理的文件 | `Source/`、`Config/`、`*.uproject`、`.git/`、`.gitignore` |
| UE 生成物 | `Binaries/`、`DerivedDataCache/`、`Intermediate/`、`Saved/`、`Build/` |
| IDE 文件 | `.vs/`、`.idea/`、`*.sln`、`*.vcxproj` |
| 本地配置 | `.p4config`、`.p4ignore`、`p4login.ps1`、`opencode.json`、`.opencode/` |
| 系统垃圾 | `Thumbs.db`、`Desktop.ini`、`.DS_Store` |

### UE 编辑器 Source Control 集成（可选）

1. UE 编辑器 → Edit → Project Settings → Source Control
2. Provider 选择 **Perforce**
3. 填入 Server、User、Workspace 信息
4. 之后在 Content Browser 中右键素材即可直接 Check Out / Submit

---

## 项目结构简介

```
LetsDeepRock/
├── Source/                    # C++ 源码（Git 管理）
│   ├── LetsDeepRock/          #   主模块
│   ├── LetsDeepRock.Target.cs
│   └── LetsDeepRockEditor.Target.cs
├── Config/                    # 项目配置（Git 管理）
│   ├── DefaultEngine.ini
│   ├── DefaultGame.ini
│   ├── DefaultInput.ini
│   └── ...
├── Docs/                       # 版本化的架构与运行时契约
├── Content/                   # 素材资源（P4 管理，Git 忽略）
├── Tools/                     # 开发工具（Git 管理）
│   └── UnrealMCP/             #   AI 编辑器自动化 MCP Server
├── LetsDeepRock.uproject      # 项目定义（Git 管理）
├── .gitignore                 # Git 忽略规则
├── .p4ignore                  # P4 忽略规则（Git 忽略）
├── .p4config                  # P4 连接配置（Git 忽略，需自建）
├── GenerateProjectFiles.bat   # 生成 VS 工程文件
├── BuildEditor.bat            # 编译编辑器
└── RebuildEditor.bat          # 完整重建编辑器
```

### 启用的插件

| 插件 | 用途 |
|------|------|
| GameplayAbilities (GAS) | Gameplay Ability System — 技能/属性系统 |
| StateTree + GameplayStateTree | AI 行为树 |
| ModelingToolsEditorMode | 建模工具（仅编辑器） |
| Python Editor Script Plugin | Python 脚本支持（MCP Server 需要） |

---

## 架构契约

- [Host Migration Recovery Contract](Docs/HostMigrationRecoveryContract.md)：固定 `Active Host -> Backup A -> Backup B` 的恢复拓扑、Snapshot commit 规则、Roster/Pawn 生命周期、失败语义与验收边界。

---

## AI 编辑器自动化 (Unreal MCP)

项目集成了自研的 **Unreal MCP Server**，允许 AI Agent（如 OpenCode）直接操作 UE 编辑器，自动化资产创建、蓝图配置等重复性工作。

### 核心能力

| 类别 | 可执行的操作 |
|------|------------|
| **资产操作** | 创建 DataAsset、DataTable；搜索/列出资产；读取属性 |
| **蓝图操作** | 创建 Blueprint、设置 Class Defaults、添加组件、编译 |
| **GAS 系统** | 创建 GameplayEffect/GameplayAbility |
| **输入系统** | 创建 Input Action、配置 IMC 按键绑定 |
| **关卡操作** | 放置 Actor、构建 NavMesh、创建材质、创建 Blackboard |
| **通用工具** | 执行任意 Python 代码、控制台命令、保存资产 |

### 架构

```
AI Agent (OpenCode) → stdio → MCP Server (Python) → UE 5.8 Editor
                                    │
                                    ├── Python Remote Exec (UDP+TCP) ← 主通道
                                    └── Remote Control API (HTTP)    ← 补充通道
```

### 快速启用

**1. 安装 MCP Server 依赖：**

```powershell
cd Tools/UnrealMCP
pip install -e .
```

**2. UE 编辑器中启用插件：**

- Edit → Plugins → 启用 `Python Editor Script Plugin`
- Project Settings → Plugins → Python → Enable Remote Execution = **True**
- Multicast Bind Address 改为 `0.0.0.0`
- 可选：启用 `Remote Control API` 插件

**3. 重启编辑器后即可使用。**

> 完整配置说明见 [`Tools/UnrealMCP/README.md`](Tools/UnrealMCP/README.md)

---

## 常见问题

### Q: 克隆后 UE 编辑器报 "Missing Content" 错误？

A: 需要先完成 P4 素材同步（步骤 2.5），`Content/` 不在 Git 中。

### Q: 编译报错 "无法找到 UE 头文件"？

A: 确保已安装 UE 5.8 并关联了引擎版本。右键 `.uproject` → Switch Unreal Engine Version → 选择 5.8。

### Q: P4 命令行报 "Perforce client error: Connect to server failed"？

A: 检查 `.p4config` 中的 `P4PORT` 地址是否正确，以及网络是否能访问 `ssl:symb0x76.top:1666`。

### Q: 切换分支后 VS 工程文件过期？

A: 重新运行 `GenerateProjectFiles.bat` 或右键 `.uproject` → "Generate Visual Studio project files"。

### Q: 如何同时使用 Git 和 P4 而不冲突？

A: 两者工作在同一目录但管理不同文件。`.gitignore` 排除了 `Content/`，`.p4ignore` 排除了 `Source/`/`Config/`。只要遵守"代码走 Git，素材走 P4"原则即可。

---

## 联系

如有环境搭建问题，请联系项目管理员获取 P4 账号和权限。
