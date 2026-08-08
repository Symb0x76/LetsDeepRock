# UMGForge Core

确定性的 `.umgforge` 文档编辑内核。GUI 与 MCP 应只通过 `EditorCore.execute()` 提交强类型命令，并使用 `expectedRevision` 处理并发。

内核不执行脚本、不连接 Unreal Engine，也不包含 AI 能力。`serializeDocument()` 固定使用稳定键顺序、2 空格缩进与 CRLF。
