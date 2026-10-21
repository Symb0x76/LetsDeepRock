import { mkdtemp, rm } from "node:fs/promises";
import { tmpdir } from "node:os";
import path from "node:path";
import { afterEach, describe, expect, it } from "vitest";
import { Operations } from "../src/operations.js";

const workspaces: string[] = [];
afterEach(async () => { await Promise.all(workspaces.splice(0).map(directory => rm(directory, { recursive: true, force: true }))); });

describe("Operations", () => {
  it("支持确定性创建、dryRun 和 revision 冲突", async () => {
    const workspace = await mkdtemp(path.join(tmpdir(), "umgforge-"));
    workspaces.push(workspace);
    const operations = new Operations(workspace);
    const dry = await operations.execute("create_widget", { expectedRevision: 0, dryRun: true, parentId: "root", name: "Preview", class: "TextBlock" });
    expect(dry.revision).toBe(1);
    expect(operations.getEditor().getDocument().revision).toBe(0);
    const created = await operations.execute("create_widget", { expectedRevision: 0, parentId: "root", name: "Actual", class: "TextBlock" });
    expect(created.document?.root.children[0]?.id).toBe("node_2");
    await expect(operations.execute("delete_widget", { expectedRevision: 0, nodeId: "node_2" })).rejects.toMatchObject({ status: 409 });
  });
});
