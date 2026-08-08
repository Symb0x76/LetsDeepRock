import type { UMGForgeDocument } from "./types.js";

function stable(value: unknown): unknown {
  if (Array.isArray(value)) return value.map(stable);
  if (value !== null && typeof value === "object") {
    const source = value as Record<string, unknown>;
    const result: Record<string, unknown> = {};
    for (const key of Object.keys(source).sort()) result[key] = stable(source[key]);
    return result;
  }
  return value;
}

export function serializeDocument(document: UMGForgeDocument): string {
  return JSON.stringify(stable(document), null, 2).replace(/\n/g, "\r\n") + "\r\n";
}

export function cloneDocument(document: UMGForgeDocument): UMGForgeDocument {
  return structuredClone(document);
}
