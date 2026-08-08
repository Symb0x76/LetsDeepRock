import { DOCUMENT_VERSION, type UMGForgeDocument, type WidgetClass, type WidgetNode } from "./types.js";
import { ValidationError } from "./errors.js";

const classes = new Set<WidgetClass>([
  "CanvasPanel", "HorizontalBox", "VerticalBox", "Overlay", "SizeBox", "Border",
  "Image", "TextBlock", "Button", "WidgetSwitcher"
]);
const leaves = new Set<WidgetClass>(["Image", "TextBlock"]);
const singleChild = new Set<WidgetClass>(["SizeBox", "Border", "Button"]);

export function collectNodes(root: WidgetNode): Map<string, WidgetNode> {
  const nodes = new Map<string, WidgetNode>();
  const visit = (node: WidgetNode): void => {
    if (!nodes.has(node.id)) nodes.set(node.id, node);
    for (const child of node.children) visit(child);
  };
  visit(root);
  return nodes;
}

export function preflight(document: UMGForgeDocument): string[] {
  const issues: string[] = [];
  if (document.version !== DOCUMENT_VERSION) issues.push(`Unsupported version: ${document.version}`);
  if (!Number.isSafeInteger(document.revision) || document.revision < 0) issues.push("revision must be a non-negative safe integer");
  const ids = new Set<string>();
  const names = new Set<string>();
  const nodes: WidgetNode[] = [];
  const visit = (node: WidgetNode, ancestors: Set<WidgetNode>): void => {
    nodes.push(node);
    if (!node.id) issues.push("Node id must not be empty");
    else if (ids.has(node.id)) issues.push(`Duplicate node id: ${node.id}`);
    ids.add(node.id);
    if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(node.name)) issues.push(`Invalid node name: ${node.name}`);
    else if (names.has(node.name)) issues.push(`Duplicate node name: ${node.name}`);
    names.add(node.name);
    if (!classes.has(node.class)) issues.push(`Unsupported widget class: ${String(node.class)}`);
    if (leaves.has(node.class) && node.children.length > 0) issues.push(`${node.class} ${node.id} cannot have children`);
    if (singleChild.has(node.class) && node.children.length > 1) issues.push(`${node.class} ${node.id} can have at most one child`);
    if (ancestors.has(node)) {
      issues.push(`Cycle detected at node: ${node.id}`);
      return;
    }
    const next = new Set(ancestors).add(node);
    for (const child of node.children) visit(child, next);
  };
  visit(document.root, new Set());
  for (const node of nodes) {
    for (const interaction of node.interactions) {
      if (interaction.type === "EmitEvent") {
        if (!interaction.event.trim()) issues.push(`Empty event on node: ${node.id}`);
        continue;
      }
      const target = nodes.find(candidate => candidate.id === interaction.targetId);
      if (!target) issues.push(`Missing interaction target: ${interaction.targetId}`);
      if (interaction.type === "SetSwitcherIndex") {
        if (target && target.class !== "WidgetSwitcher") issues.push(`Target ${target.id} is not a WidgetSwitcher`);
        if (!Number.isInteger(interaction.index) || interaction.index < 0 || (target && interaction.index >= target.children.length))
          issues.push(`Invalid switcher index for target: ${interaction.targetId}`);
      }
    }
  }
  return issues;
}

export function validateDocument(document: UMGForgeDocument): void {
  const issues = preflight(document);
  if (issues.length) throw new ValidationError(issues);
}
