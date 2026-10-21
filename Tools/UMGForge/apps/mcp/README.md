# UMGForge MCP Server

基于 `@umgforge/core` 的本地 MCP Server 与 HTTP bridge。两个入口共享同一个操作状态，不执行任意脚本，也不调用 Unreal Engine。

## 要求

- Node.js 20 或更高版本
- 从 UMGForge workspace 安装好的依赖

## 启动

```powershell
npm start --workspace @umgforge/mcp -- --workspace D:\path\to\documents
```

服务同时启用 stdio MCP 和 `http://127.0.0.1:4763`。HTTP CORS 仅接受 `localhost` 与 `127.0.0.1` origin。日志只写入 stderr。

`--workspace` 是必填参数。`save_document` 和 `load_document` 只接受该目录内的相对路径，并检查解析后的真实路径以阻止目录穿越。保存使用 core 的稳定排序、CRLF 序列化。

## MCP 工具

`get_document`、`get_selection`、`create_widget`、`update_widget`、`move_widget`、`delete_widget`、`set_interactions`、`select`、`validate`、`undo`、`redo`、`save_document`、`load_document`。

写操作使用 `expectedRevision` 进行乐观并发控制。创建、更新、移动、删除和交互设置支持 `dryRun`。调用方可传入节点 `id`；省略时使用进程内单调计数器生成 `node_1`、`node_2` 等 ID。

## HTTP API

- `GET /status`
- `GET /document`
- `POST /commands`，body 为 `{ "operation": "create_widget", "input": { ... } }`
- `POST /undo`
- `POST /redo`
- `GET /preflight`
- `POST /save`
- `POST /load`

业务错误使用 HTTP 4xx；MCP 返回 `isError: true`。成功响应包含 `revision`、`errors`，事务响应还包含 `diff`。

## OpenCode MCP 配置示例

```json
{
  "mcp": {
    "umgforge": {
      "type": "local",
      "command": [
        "npm",
        "start",
        "--workspace",
        "@umgforge/mcp",
        "--",
        "--workspace",
        "D:\\ui-documents"
      ],
      "enabled": true
    }
  }
}
```

## 验证

```powershell
npm test --workspace @umgforge/mcp
npm run typecheck --workspace @umgforge/mcp
```
