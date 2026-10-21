# UMGForge

UMGForge 是面向 Unreal UMG 的确定性 UI 文档编辑原型。当前阶段已经具备 React GUI editor、共享的 `@umgforge/core` 编辑内核，以及同时提供 stdio MCP 和 localhost HTTP bridge 的 MCP Server。

当前**尚未实现 Unreal Engine UMG/WBP importer**，因此工具不会创建或修改 UE Widget Blueprint 资产。

## 架构原则

- GUI、AI 与 MCP 只读写版本化的 UMGForge 文档，并通过共享 `EditorCore`、强类型命令、revision、校验和 undo/redo 工作。
- AI/MCP 不直接操作 Unreal Engine，也不负责生成 WBP。
- 未来的 UMG/WBP importer 必须是从已校验文档到 UE 资产的确定性转换器；相同输入必须产生相同结果，生成阶段不得依赖 AI。
- 文档序列化使用稳定键顺序、2 空格缩进和 CRLF。

## 要求与安装

- Node.js 20 或更高版本
- npm workspace

```powershell
npm install
```

## 启动

启动 editor：

```powershell
npm run dev:editor
```

启动 MCP Server 与 `http://127.0.0.1:4763` bridge（`--workspace` 必填）：

```powershell
npm run start:mcp -- --workspace D:\path\to\umgforge-documents
```

OpenCode 的本地 MCP 配置见 [apps/mcp/README.md#opencode-mcp-配置示例](apps/mcp/README.md#opencode-mcp-配置示例)。

## 验证

```powershell
npm test
npm run build
npm run typecheck
```

## 支持范围

控件：`CanvasPanel`、`HorizontalBox`、`VerticalBox`、`Overlay`、`SizeBox`、`Border`、`Image`、`TextBlock`、`Button`、`WidgetSwitcher`。

交互：`SetSwitcherIndex`、`SetVisibility`、`EmitEvent`。

## HTTP bridge

- `GET /status`
- `GET /document`
- `GET /preflight`
- `POST /commands`
- `POST /undo`
- `POST /redo`
- `POST /save`
- `POST /load`

`POST /commands` 每次提交一个 MCP 操作，body 与服务实现一致：

```json
{
  "operation": "create_widget",
  "input": {
    "expectedRevision": 0,
    "parentId": "root",
    "name": "Button_1",
    "class": "Button"
  }
}
```

详细工具、并发和路径安全说明见 [apps/mcp/README.md](apps/mcp/README.md)。
