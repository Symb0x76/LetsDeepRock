export const DOCUMENT_VERSION = "1.0" as const;

export type WidgetClass =
  | "CanvasPanel" | "HorizontalBox" | "VerticalBox" | "Overlay"
  | "SizeBox" | "Border" | "Image" | "TextBlock" | "Button" | "WidgetSwitcher";

export type Visibility = "Visible" | "Collapsed" | "Hidden" | "HitTestInvisible" | "SelfHitTestInvisible";

export type Interaction =
  | { type: "SetSwitcherIndex"; targetId: string; index: number }
  | { type: "SetVisibility"; targetId: string; visibility: Visibility }
  | { type: "EmitEvent"; event: string; payload?: Record<string, unknown> };

export interface WidgetNode {
  id: string;
  name: string;
  class: WidgetClass;
  slot: Record<string, unknown> | null;
  props: Record<string, unknown>;
  children: WidgetNode[];
  interactions: Interaction[];
}

export interface UMGForgeDocument {
  version: typeof DOCUMENT_VERSION;
  revision: number;
  root: WidgetNode;
}

export type NodePatch = Partial<Pick<WidgetNode, "name" | "class" | "slot" | "props">>;

export type Command =
  | { type: "createNode"; parentId: string; index?: number; node: WidgetNode }
  | { type: "deleteNode"; nodeId: string }
  | { type: "moveNode"; nodeId: string; parentId: string; index?: number }
  | { type: "updateNode"; nodeId: string; patch: NodePatch }
  | { type: "setInteractions"; nodeId: string; interactions: Interaction[] }
  | { type: "setSelection"; nodeIds: string[] };

export interface ExecuteOptions {
  expectedRevision: number;
  dryRun?: boolean;
}

export interface TransactionDiff {
  revisionBefore: number;
  revisionAfter: number;
  commands: Command[];
  before: string;
  after: string;
}

export interface TransactionResult {
  document: UMGForgeDocument;
  selection: string[];
  diff: TransactionDiff;
  dryRun: boolean;
}
