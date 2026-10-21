import path from "node:path";
import { Operations } from "./operations.js";
import { createBridge, listenBridge } from "./http.js";
import { connectStdio, createMcpServer } from "./mcp.js";

function workspaceArgument(argv: string[]): string {
  const index = argv.indexOf("--workspace");
  const value = index >= 0 ? argv[index + 1] : undefined;
  if (!value) throw new Error("Required argument: --workspace <directory>");
  return path.resolve(value);
}

async function main(): Promise<void> {
  const operations = new Operations(workspaceArgument(process.argv.slice(2)));
  const bridge = createBridge(operations);
  await listenBridge(bridge);
  console.error("UMGForge HTTP bridge listening on http://127.0.0.1:4763");
  await connectStdio(createMcpServer(operations));
}

main().catch(error => {
  console.error(error instanceof Error ? error.stack ?? error.message : String(error));
  process.exitCode = 1;
});
