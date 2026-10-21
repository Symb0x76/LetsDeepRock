import { McpServer, type CallToolResult } from "@modelcontextprotocol/server";
import { StdioServerTransport } from "@modelcontextprotocol/server/stdio";
import { schemas, type OperationName } from "./schemas.js";
import { OperationError, type Operations } from "./operations.js";

function success(result: Awaited<ReturnType<Operations["execute"]>>): CallToolResult {
  return {
    content: [{ type: "text", text: JSON.stringify(result) }],
    structuredContent: result
  };
}

function failure(error: unknown): CallToolResult {
  const mapped = error instanceof OperationError
    ? error
    : new OperationError(500, "internal_error", error instanceof Error ? error.message : "Unknown error");
  const result = { error: mapped.code, errors: mapped.errors };
  return {
    isError: true,
    content: [{ type: "text", text: JSON.stringify(result) }],
    structuredContent: result
  };
}

async function executeTool(operations: Operations, name: OperationName, input: unknown): Promise<CallToolResult> {
  try {
    return success(await operations.execute(name, input));
  } catch (error) {
    return failure(error);
  }
}

export function createMcpServer(operations: Operations): McpServer {
  const server = new McpServer({ name: "umgforge", version: "0.1.0" });
  server.registerTool("get_document", { description: "UMGForge get_document", inputSchema: schemas.get_document }, input => executeTool(operations, "get_document", input));
  server.registerTool("get_selection", { description: "UMGForge get_selection", inputSchema: schemas.get_selection }, input => executeTool(operations, "get_selection", input));
  server.registerTool("create_widget", { description: "UMGForge create_widget", inputSchema: schemas.create_widget }, input => executeTool(operations, "create_widget", input));
  server.registerTool("update_widget", { description: "UMGForge update_widget", inputSchema: schemas.update_widget }, input => executeTool(operations, "update_widget", input));
  server.registerTool("move_widget", { description: "UMGForge move_widget", inputSchema: schemas.move_widget }, input => executeTool(operations, "move_widget", input));
  server.registerTool("delete_widget", { description: "UMGForge delete_widget", inputSchema: schemas.delete_widget }, input => executeTool(operations, "delete_widget", input));
  server.registerTool("set_interactions", { description: "UMGForge set_interactions", inputSchema: schemas.set_interactions }, input => executeTool(operations, "set_interactions", input));
  server.registerTool("select", { description: "UMGForge select", inputSchema: schemas.select }, input => executeTool(operations, "select", input));
  server.registerTool("validate", { description: "UMGForge validate", inputSchema: schemas.validate }, input => executeTool(operations, "validate", input));
  server.registerTool("undo", { description: "UMGForge undo", inputSchema: schemas.undo }, input => executeTool(operations, "undo", input));
  server.registerTool("redo", { description: "UMGForge redo", inputSchema: schemas.redo }, input => executeTool(operations, "redo", input));
  server.registerTool("save_document", { description: "UMGForge save_document", inputSchema: schemas.save_document }, input => executeTool(operations, "save_document", input));
  server.registerTool("load_document", { description: "UMGForge load_document", inputSchema: schemas.load_document }, input => executeTool(operations, "load_document", input));
  return server;
}

export async function connectStdio(server: McpServer): Promise<void> {
  await server.connect(new StdioServerTransport());
}
