import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import type { AddressInfo } from "node:net";
import { afterEach, expect, it } from "vitest";
import { createBridge, listenBridge } from "../src/http.js";
import { Operations } from "../src/operations.js";

const cleanups: (() => Promise<void>)[] = [];
afterEach(async () => { await Promise.all(cleanups.splice(0).map(cleanup => cleanup())); });

it("HTTP 与直接操作共享同一编辑状态", async () => {
  const workspace = await mkdtemp(path.join(tmpdir(), "umgforge-http-"));
  const operations = new Operations(workspace);
  const server = createBridge(operations);
  await listenBridge(server, 0);
  cleanups.push(async () => { await new Promise<void>(resolve => server.close(() => resolve())); await rm(workspace, { recursive: true, force: true }); });
  const port = (server.address() as AddressInfo).port;
  const response = await fetch(`http://127.0.0.1:${port}/commands`, {
    method: "POST",
    headers: { "content-type": "application/json", origin: "http://localhost" },
    body: JSON.stringify({ operation: "create_widget", input: { expectedRevision: 0, parentId: "root", id: "shared", name: "Shared", class: "Button" } })
  });
  expect(response.status).toBe(200);
  expect(operations.getEditor().getDocument().root.children[0]?.id).toBe("shared");
  const document = await (await fetch(`http://127.0.0.1:${port}/document`)).json() as { revision: number };
  expect(document.revision).toBe(1);
});
