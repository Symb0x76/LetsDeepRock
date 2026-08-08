import { promises as fs } from "node:fs";
import path from "node:path";
import {
  EditorCore,
  RevisionConflictError,
  ValidationError,
  serializeDocument,
  type Command,
  type UMGForgeDocument,
  type WidgetNode
} from "@umgforge/core";
import { ZodError } from "zod";
import { documentSchema, schemas, type OperationName } from "./schemas.js";

export interface OperationResult {
  revision: number;
  diff?: unknown;
  document?: UMGForgeDocument;
  selection?: string[];
  errors: string[];
  dryRun?: boolean;
  path?: string;
}

export class OperationError extends Error {
  constructor(public readonly status: number, public readonly code: string, message: string, public readonly errors: string[] = [message]) {
    super(message);
    this.name = "OperationError";
  }
}

function mapError(error: unknown): OperationError {
  if (error instanceof OperationError) return error;
  if (error instanceof RevisionConflictError) return new OperationError(409, "revision_conflict", error.message);
  if (error instanceof ValidationError) return new OperationError(400, "validation_error", error.message, error.issues);
  if (error instanceof ZodError) return new OperationError(400, "invalid_input", "Input validation failed", error.issues.map(issue => `${issue.path.join(".")}: ${issue.message}`));
  if (error instanceof Error && "code" in error && (error as NodeJS.ErrnoException).code === "ENOENT")
    return new OperationError(404, "not_found", error.message);
  return new OperationError(500, "internal_error", error instanceof Error ? error.message : "Unknown error");
}

export function initialDocument(): UMGForgeDocument {
  return {
    version: "1.0",
    revision: 0,
    root: { id: "root", name: "Root", class: "CanvasPanel", slot: null, props: {}, children: [], interactions: [] }
  };
}

export class Operations {
  private editor: EditorCore;
  private nextNodeNumber = 1;
  private readonly workspace: string;

  constructor(workspace: string, editor = new EditorCore(initialDocument())) {
    this.workspace = path.resolve(workspace);
    this.editor = editor;
  }

  getEditor(): EditorCore { return this.editor; }

  async execute(name: OperationName, rawInput: unknown): Promise<OperationResult> {
    try {
      const input = schemas[name].parse(rawInput) as Record<string, unknown>;
      switch (name) {
        case "get_document": return this.readDocument();
        case "get_selection": return { revision: this.editor.getDocument().revision, selection: this.editor.getSelection(), errors: [] };
        case "validate": return { revision: this.editor.getDocument().revision, errors: this.editor.preflight() };
        case "create_widget": return this.create(input);
        case "update_widget": return this.run({ type: "updateNode", nodeId: input.nodeId as string, patch: input.patch as never }, input);
        case "move_widget": return this.run({ type: "moveNode", nodeId: input.nodeId as string, parentId: input.parentId as string, ...(input.index === undefined ? {} : { index: input.index as number }) }, input);
        case "delete_widget": return this.run({ type: "deleteNode", nodeId: input.nodeId as string }, input);
        case "set_interactions": return this.run({ type: "setInteractions", nodeId: input.nodeId as string, interactions: input.interactions as never }, input);
        case "select": return this.run({ type: "setSelection", nodeIds: input.nodeIds as string[] }, input);
        case "undo": return this.history("undo", input.expectedRevision as number);
        case "redo": return this.history("redo", input.expectedRevision as number);
        case "save_document": return await this.save(input.path as string, input.expectedRevision as number);
        case "load_document": return await this.load(input.path as string, input.expectedRevision as number);
      }
    } catch (error) {
      throw mapError(error);
    }
  }

  private readDocument(): OperationResult {
    const document = this.editor.getDocument();
    return { revision: document.revision, document, selection: this.editor.getSelection(), errors: [] };
  }

  private create(input: Record<string, unknown>): OperationResult {
    const id = (input.id as string | undefined) ?? this.allocateNodeId();
    const node: WidgetNode = {
      id,
      name: input.name as string,
      class: input.class as WidgetNode["class"],
      slot: (input.slot as Record<string, unknown> | null | undefined) ?? null,
      props: (input.props as Record<string, unknown> | undefined) ?? {},
      children: [],
      interactions: (input.interactions as WidgetNode["interactions"] | undefined) ?? []
    };
    return this.run({ type: "createNode", parentId: input.parentId as string, node, ...(input.index === undefined ? {} : { index: input.index as number }) }, input);
  }

  private allocateNodeId(): string {
    const ids = new Set<string>();
    const visit = (node: WidgetNode): void => { ids.add(node.id); node.children.forEach(visit); };
    visit(this.editor.getDocument().root);
    let id: string;
    do id = `node_${this.nextNodeNumber++}`; while (ids.has(id));
    return id;
  }

  private run(command: Command, input: Record<string, unknown>): OperationResult {
    const result = this.editor.execute(command, {
      expectedRevision: input.expectedRevision as number,
      ...(input.dryRun === undefined ? {} : { dryRun: input.dryRun as boolean })
    });
    return { revision: result.document.revision, document: result.document, selection: result.selection, diff: result.diff, dryRun: result.dryRun, errors: [] };
  }

  private history(direction: "undo" | "redo", expectedRevision: number): OperationResult {
    const before = serializeDocument(this.editor.getDocument());
    const document = this.editor[direction](expectedRevision);
    return { revision: document.revision, document, selection: this.editor.getSelection(), diff: { before, after: serializeDocument(document) }, errors: [] };
  }

  private checkRevision(expectedRevision: number): void {
    const actual = this.editor.getDocument().revision;
    if (actual !== expectedRevision) throw new RevisionConflictError(expectedRevision, actual);
  }

  private async safePath(relativePath: string, mustExist: boolean): Promise<string> {
    if (path.isAbsolute(relativePath)) throw new OperationError(403, "path_forbidden", "Path must be relative to workspace");
    const target = path.resolve(this.workspace, relativePath);
    const relative = path.relative(this.workspace, target);
    if (relative.startsWith("..") || path.isAbsolute(relative)) throw new OperationError(403, "path_forbidden", "Path escapes workspace");
    const checked = mustExist ? await fs.realpath(target) : path.join(await fs.realpath(path.dirname(target)), path.basename(target));
    const realWorkspace = await fs.realpath(this.workspace);
    const realRelative = path.relative(realWorkspace, checked);
    if (realRelative.startsWith("..") || path.isAbsolute(realRelative)) throw new OperationError(403, "path_forbidden", "Resolved path escapes workspace");
    return checked;
  }

  private async save(relativePath: string, expectedRevision: number): Promise<OperationResult> {
    this.checkRevision(expectedRevision);
    const target = await this.safePath(relativePath, false);
    await fs.writeFile(target, serializeDocument(this.editor.getDocument()), "utf8");
    return { ...this.readDocument(), diff: { type: "saveDocument", path: relativePath, revision: expectedRevision }, path: relativePath };
  }

  private async load(relativePath: string, expectedRevision: number): Promise<OperationResult> {
    this.checkRevision(expectedRevision);
    const target = await this.safePath(relativePath, true);
    const before = serializeDocument(this.editor.getDocument());
    const parsed = documentSchema.parse(JSON.parse(await fs.readFile(target, "utf8"))) as UMGForgeDocument;
    this.editor = new EditorCore(parsed);
    this.seedCounter(parsed.root);
    return { ...this.readDocument(), diff: { type: "loadDocument", path: relativePath, before, after: serializeDocument(parsed) }, path: relativePath };
  }

  private seedCounter(root: WidgetNode): void {
    const visit = (node: WidgetNode): void => {
      const match = /^node_(\d+)$/.exec(node.id);
      if (match) this.nextNodeNumber = Math.max(this.nextNodeNumber, Number(match[1]) + 1);
      node.children.forEach(visit);
    };
    visit(root);
  }
}
