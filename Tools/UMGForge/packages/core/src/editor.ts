import { RevisionConflictError, ValidationError } from "./errors.js";
import { cloneDocument, serializeDocument } from "./serialize.js";
import { collectNodes, preflight, validateDocument } from "./validate.js";
import type { Command, ExecuteOptions, TransactionResult, UMGForgeDocument, WidgetNode } from "./types.js";

interface Snapshot { document: UMGForgeDocument; selection: string[] }

function findParent(root: WidgetNode, id: string): WidgetNode | undefined {
  if (root.children.some(child => child.id === id)) return root;
  for (const child of root.children) {
    const found = findParent(child, id);
    if (found) return found;
  }
  return undefined;
}

function insert(parent: WidgetNode, node: WidgetNode, index?: number): void {
  const at = index === undefined ? parent.children.length : index;
  if (!Number.isInteger(at) || at < 0 || at > parent.children.length) throw new ValidationError([`Invalid child index: ${at}`]);
  parent.children.splice(at, 0, node);
}

export class EditorCore {
  private document: UMGForgeDocument;
  private selection: string[] = [];
  private readonly undoStack: Snapshot[] = [];
  private readonly redoStack: Snapshot[] = [];

  constructor(document: UMGForgeDocument) {
    validateDocument(document);
    this.document = cloneDocument(document);
  }

  getDocument(): UMGForgeDocument { return cloneDocument(this.document); }
  getSelection(): string[] { return [...this.selection]; }
  preflight(): string[] { return preflight(this.document); }

  execute(commands: Command | Command[], options: ExecuteOptions): TransactionResult {
    if (options.expectedRevision !== this.document.revision)
      throw new RevisionConflictError(options.expectedRevision, this.document.revision);
    const list = Array.isArray(commands) ? commands : [commands];
    const working = cloneDocument(this.document);
    let nextSelection = [...this.selection];
    for (const command of list) nextSelection = this.apply(working, nextSelection, command);
    working.revision++;
    validateDocument(working);
    const before = serializeDocument(this.document);
    const after = serializeDocument(working);
    const result: TransactionResult = {
      document: cloneDocument(working),
      selection: [...nextSelection],
      diff: { revisionBefore: this.document.revision, revisionAfter: working.revision, commands: structuredClone(list), before, after },
      dryRun: options.dryRun === true
    };
    if (!options.dryRun) {
      this.undoStack.push({ document: cloneDocument(this.document), selection: [...this.selection] });
      this.document = working;
      this.selection = nextSelection;
      this.redoStack.length = 0;
    }
    return result;
  }

  undo(expectedRevision: number): UMGForgeDocument {
    this.checkRevision(expectedRevision);
    const previous = this.undoStack.pop();
    if (!previous) throw new ValidationError(["Nothing to undo"]);
    this.redoStack.push({ document: cloneDocument(this.document), selection: [...this.selection] });
    const revision = this.document.revision + 1;
    this.document = cloneDocument(previous.document);
    this.document.revision = revision;
    this.selection = [...previous.selection];
    return this.getDocument();
  }

  redo(expectedRevision: number): UMGForgeDocument {
    this.checkRevision(expectedRevision);
    const next = this.redoStack.pop();
    if (!next) throw new ValidationError(["Nothing to redo"]);
    this.undoStack.push({ document: cloneDocument(this.document), selection: [...this.selection] });
    const revision = this.document.revision + 1;
    this.document = cloneDocument(next.document);
    this.document.revision = revision;
    this.selection = [...next.selection];
    return this.getDocument();
  }

  private checkRevision(expected: number): void {
    if (expected !== this.document.revision) throw new RevisionConflictError(expected, this.document.revision);
  }

  private apply(document: UMGForgeDocument, selection: string[], command: Command): string[] {
    const nodes = collectNodes(document.root);
    switch (command.type) {
      case "createNode": {
        const parent = nodes.get(command.parentId);
        if (!parent) throw new ValidationError([`Parent not found: ${command.parentId}`]);
        insert(parent, structuredClone(command.node), command.index);
        return selection;
      }
      case "deleteNode": {
        if (command.nodeId === document.root.id) throw new ValidationError(["Cannot delete root node"]);
        const parent = findParent(document.root, command.nodeId);
        if (!parent) throw new ValidationError([`Node not found: ${command.nodeId}`]);
        parent.children.splice(parent.children.findIndex(child => child.id === command.nodeId), 1);
        const remaining = collectNodes(document.root);
        return selection.filter(id => remaining.has(id));
      }
      case "moveNode": {
        if (command.nodeId === document.root.id) throw new ValidationError(["Cannot move root node"]);
        const node = nodes.get(command.nodeId);
        const oldParent = findParent(document.root, command.nodeId);
        const newParent = nodes.get(command.parentId);
        if (!node || !oldParent || !newParent) throw new ValidationError(["Move source or parent not found"]);
        if (collectNodes(node).has(command.parentId)) throw new ValidationError(["Cannot move a node into its own subtree"]);
        oldParent.children.splice(oldParent.children.indexOf(node), 1);
        insert(newParent, node, command.index);
        return selection;
      }
      case "updateNode": {
        const node = nodes.get(command.nodeId);
        if (!node) throw new ValidationError([`Node not found: ${command.nodeId}`]);
        Object.assign(node, structuredClone(command.patch));
        return selection;
      }
      case "setInteractions": {
        const node = nodes.get(command.nodeId);
        if (!node) throw new ValidationError([`Node not found: ${command.nodeId}`]);
        node.interactions = structuredClone(command.interactions);
        return selection;
      }
      case "setSelection": {
        const missing = command.nodeIds.filter(id => !nodes.has(id));
        if (missing.length) throw new ValidationError(missing.map(id => `Selection node not found: ${id}`));
        return [...new Set(command.nodeIds)];
      }
    }
  }
}
