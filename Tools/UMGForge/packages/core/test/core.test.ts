import { describe, expect, it } from "vitest";
import { DOCUMENT_VERSION, EditorCore, RevisionConflictError, ValidationError, preflight, serializeDocument, type UMGForgeDocument, type WidgetNode } from "../src/index.js";

const node = (id: string, name: string, klass: WidgetNode["class"] = "CanvasPanel", children: WidgetNode[] = []): WidgetNode => ({
  id, name, class: klass, slot: null, props: {}, children, interactions: []
});
const document = (): UMGForgeDocument => ({ version: DOCUMENT_VERSION, revision: 0, root: node("root", "Root", "CanvasPanel") });

describe("EditorCore", () => {
  it("executes commands atomically and supports dry-run", () => {
    const core = new EditorCore(document());
    const result = core.execute([
      { type: "createNode", parentId: "root", node: node("switcher", "Switcher", "WidgetSwitcher", [node("page", "Page", "Overlay")]) },
      { type: "setInteractions", nodeId: "root", interactions: [{ type: "SetSwitcherIndex", targetId: "switcher", index: 0 }] },
      { type: "setSelection", nodeIds: ["switcher"] }
    ], { expectedRevision: 0, dryRun: true });
    expect(result.document.revision).toBe(1);
    expect(result.diff.before).not.toBe(result.diff.after);
    expect(core.getDocument().revision).toBe(0);
    core.execute(result.diff.commands, { expectedRevision: 0 });
    expect(core.getSelection()).toEqual(["switcher"]);
  });

  it("creates, updates, moves and deletes nodes", () => {
    const core = new EditorCore(document());
    core.execute([
      { type: "createNode", parentId: "root", node: node("a", "A", "VerticalBox") },
      { type: "createNode", parentId: "root", node: node("b", "B", "Overlay") },
      { type: "createNode", parentId: "a", node: node("text", "Label", "TextBlock") },
      { type: "moveNode", nodeId: "text", parentId: "b" },
      { type: "updateNode", nodeId: "text", patch: { props: { text: "Hello" } } },
      { type: "deleteNode", nodeId: "a" }
    ], { expectedRevision: 0 });
    expect(core.getDocument().root.children[0]?.id).toBe("b");
    expect(core.getDocument().root.children[0]?.children[0]?.props).toEqual({ text: "Hello" });
  });

  it("undoes and redoes with monotonic revisions", () => {
    const core = new EditorCore(document());
    core.execute({ type: "createNode", parentId: "root", node: node("x", "X") }, { expectedRevision: 0 });
    expect(core.undo(1).root.children).toHaveLength(0);
    expect(core.redo(2).root.children).toHaveLength(1);
    expect(core.getDocument().revision).toBe(3);
  });

  it("rejects revision conflicts", () => {
    const core = new EditorCore(document());
    expect(() => core.execute({ type: "setSelection", nodeIds: [] }, { expectedRevision: 9 })).toThrow(RevisionConflictError);
  });

  it("validates structure, names and references", () => {
    const invalid = document();
    invalid.root.children = [node("same", "Duplicate", "Image", [node("same", "Duplicate", "TextBlock")])];
    invalid.root.interactions = [{ type: "SetVisibility", targetId: "missing", visibility: "Hidden" }];
    expect(preflight(invalid)).toEqual(expect.arrayContaining([
      "Duplicate node id: same", "Duplicate node name: Duplicate", "Image same cannot have children", "Missing interaction target: missing"
    ]));
    expect(() => new EditorCore(invalid)).toThrow(ValidationError);
  });

  it("rejects moving a node into its own subtree", () => {
    const source = document();
    source.root.children = [node("parent", "Parent", "VerticalBox", [node("child", "Child", "Overlay")])];
    const core = new EditorCore(source);
    expect(() => core.execute({ type: "moveNode", nodeId: "parent", parentId: "child" }, { expectedRevision: 0 }))
      .toThrow("Cannot move a node into its own subtree");
  });

  it("serializes deterministically with CRLF", () => {
    const a = document();
    a.root.props = { z: 1, a: { y: 2, b: 3 } };
    const b = document();
    b.root.props = { a: { b: 3, y: 2 }, z: 1 };
    const output = serializeDocument(a);
    expect(output).toBe(serializeDocument(b));
    expect(output).not.toMatch(/(^|[^\r])\n/);
    expect(output.endsWith("\r\n")).toBe(true);
  });
});
