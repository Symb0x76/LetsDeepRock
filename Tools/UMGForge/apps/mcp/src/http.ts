import { createServer, type IncomingMessage, type Server, type ServerResponse } from "node:http";
import { commandRequestSchema } from "./schemas.js";
import { OperationError, type Operations } from "./operations.js";

function isAllowedOrigin(origin: string): boolean {
  try {
    const url = new URL(origin);
    return (url.protocol === "http:" || url.protocol === "https:") && (url.hostname === "localhost" || url.hostname === "127.0.0.1");
  } catch {
    return false;
  }
}

function cors(request: IncomingMessage, response: ServerResponse): boolean {
  const origin = request.headers.origin;
  if (origin && !isAllowedOrigin(origin)) {
    response.writeHead(403, { "content-type": "application/json" });
    response.end(JSON.stringify({ error: "origin_forbidden" }));
    return false;
  }
  if (origin) response.setHeader("access-control-allow-origin", origin);
  response.setHeader("access-control-allow-methods", "GET, POST, OPTIONS");
  response.setHeader("access-control-allow-headers", "content-type");
  return true;
}

async function body(request: IncomingMessage): Promise<unknown> {
  const chunks: Buffer[] = [];
  let length = 0;
  for await (const chunk of request) {
    const buffer = Buffer.from(chunk);
    length += buffer.length;
    if (length > 1_048_576) throw new OperationError(413, "payload_too_large", "Request body exceeds 1 MiB");
    chunks.push(buffer);
  }
  return JSON.parse(Buffer.concat(chunks).toString("utf8") || "{}");
}

function send(response: ServerResponse, status: number, value: unknown): void {
  response.writeHead(status, { "content-type": "application/json; charset=utf-8" });
  response.end(JSON.stringify(value));
}

export function createBridge(operations: Operations): Server {
  return createServer(async (request, response) => {
    if (!cors(request, response)) return;
    if (request.method === "OPTIONS") { response.writeHead(204); response.end(); return; }
    try {
      const route = `${request.method} ${request.url}`;
      if (route === "GET /status") return send(response, 200, { ok: true, revision: operations.getEditor().getDocument().revision });
      if (route === "GET /document") return send(response, 200, await operations.execute("get_document", {}));
      if (route === "GET /preflight") return send(response, 200, await operations.execute("validate", {}));
      if (route === "POST /commands") {
        const command = commandRequestSchema.parse(await body(request));
        return send(response, 200, await operations.execute(command.operation, command.input));
      }
      const routes = {
        "POST /undo": "undo",
        "POST /redo": "redo",
        "POST /save": "save_document",
        "POST /load": "load_document"
      } as const;
      const operation = routes[route as keyof typeof routes];
      if (operation) return send(response, 200, await operations.execute(operation, await body(request)));
      send(response, 404, { error: "not_found" });
    } catch (error) {
      const mapped = error instanceof OperationError ? error : new OperationError(400, "invalid_request", error instanceof Error ? error.message : "Invalid request");
      send(response, mapped.status, { error: mapped.code, errors: mapped.errors });
    }
  });
}

export async function listenBridge(server: Server, port = 4763): Promise<void> {
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(port, "127.0.0.1", () => { server.off("error", reject); resolve(); });
  });
}
