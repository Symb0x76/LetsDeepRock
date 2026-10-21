import {
  DOCUMENT_VERSION,
  EditorCore,
  serializeDocument,
  validateDocument,
  type Command,
  type Interaction,
  type UMGForgeDocument,
  type WidgetClass,
  type WidgetNode
} from '@umgforge/core';

export * from './types';

const BRIDGE_URL = 'http://127.0.0.1:4763';

function responseDocument(value: unknown): UMGForgeDocument | null {
  if (!value || typeof value !== 'object') return null;
  const record = value as Record<string, unknown>;
  const candidate = (record.document ?? value) as UMGForgeDocument;
  try {
    validateDocument(candidate);
    return candidate;
  } catch {
    return null;
  }
}

class LocalhostBridge {
  async available(): Promise<boolean> {
    try {
      const response = await fetch(`${BRIDGE_URL}/status`, { signal: AbortSignal.timeout(400) });
      return response.ok;
    } catch {
      return false;
    }
  }

  async document(): Promise<UMGForgeDocument | null> {
    return responseDocument(await this.request('/document'));
  }

  async execute(commands: Command[], expectedRevision: number): Promise<UMGForgeDocument | null> {
    let revision = expectedRevision;
    let document: UMGForgeDocument | null = null;
    for (const command of commands) {
      const request = this.commandRequest(command, revision);
      document = responseDocument(await this.request('/commands', request));
      if (!document) return null;
      revision = document.revision;
    }
    return document;
  }

  async undo(expectedRevision: number): Promise<UMGForgeDocument | null> {
    return responseDocument(await this.request('/undo', { expectedRevision }));
  }

  async redo(expectedRevision: number): Promise<UMGForgeDocument | null> {
    return responseDocument(await this.request('/redo', { expectedRevision }));
  }

  async preflight(): Promise<string[]> {
    const value = await this.request('/preflight');
    if (Array.isArray(value)) return value.filter((issue): issue is string => typeof issue === 'string');
    if (value && typeof value === 'object') {
      const issues = (value as Record<string, unknown>).errors;
      if (Array.isArray(issues)) return issues.filter((issue): issue is string => typeof issue === 'string');
    }
    return [];
  }

  private async request(path: string, body?: unknown): Promise<unknown> {
    const response = await fetch(`${BRIDGE_URL}${path}`, {
      method: body === undefined ? 'GET' : 'POST',
      headers: body === undefined ? undefined : { 'Content-Type': 'application/json' },
      body: body === undefined ? undefined : JSON.stringify(body),
      signal: AbortSignal.timeout(1000)
    });
    if (!response.ok) throw new Error(`Bridge ${path} failed: ${response.status}`);
    return response.json();
  }

  private commandRequest(command: Command, expectedRevision: number): { operation: string; input: Record<string, unknown> } {
    switch (command.type) {
      case 'createNode':
        return {
          operation: 'create_widget',
          input: {
            expectedRevision,
            parentId: command.parentId,
            ...(command.index === undefined ? {} : { index: command.index }),
            id: command.node.id,
            name: command.node.name,
            class: command.node.class,
            slot: command.node.slot,
            props: command.node.props,
            interactions: command.node.interactions
          }
        };
      case 'updateNode':
        return { operation: 'update_widget', input: { expectedRevision, nodeId: command.nodeId, patch: command.patch } };
      case 'moveNode':
        return {
          operation: 'move_widget',
          input: { expectedRevision, nodeId: command.nodeId, parentId: command.parentId, ...(command.index === undefined ? {} : { index: command.index }) }
        };
      case 'deleteNode':
        return { operation: 'delete_widget', input: { expectedRevision, nodeId: command.nodeId } };
      case 'setInteractions':
        return { operation: 'set_interactions', input: { expectedRevision, nodeId: command.nodeId, interactions: command.interactions } };
      case 'setSelection':
        return { operation: 'select', input: { expectedRevision, nodeIds: command.nodeIds } };
    }
  }
}

export class DocumentAdapter {
  private core: EditorCore | null = null;
  private readonly bridge = new LocalhostBridge();
  private bridgeActive = false;
  private listeners: Array<() => void> = [];
  private undoDepth = 0;
  private redoDepth = 0;
  private pollTimer: ReturnType<typeof setInterval> | null = null;
  private polling = false;
  private nextNodeNumber = 1;
  private readonly nextNameNumber = new Map<WidgetClass, number>();

  loadDocument(data: UMGForgeDocument): void {
    validateDocument(data);
    this.core = new EditorCore(data);
    this.seedIdentityCounters(data.root);
    this.undoDepth = 0;
    this.redoDepth = 0;
    this.notify();
    if (!this.pollTimer) {
      void this.pollBridge();
      this.pollTimer = setInterval(() => void this.pollBridge(), 500);
    }
  }

  dispose(): void {
    if (this.pollTimer) clearInterval(this.pollTimer);
    this.pollTimer = null;
  }

  getDocument(): UMGForgeDocument | null {
    return this.core?.getDocument() ?? null;
  }

  getRevision(): number {
    return this.getDocument()?.revision ?? 0;
  }

  exportJSON(): string {
    const document = this.getDocument();
    return document ? serializeDocument(document) : '{}\r\n';
  }

  importJSON(json: string): boolean {
    try {
      const data = JSON.parse(json) as UMGForgeDocument;
      validateDocument(data);
      this.loadDocument(data);
      return true;
    } catch {
      return false;
    }
  }

  validate(): string[] {
    return this.core?.preflight() ?? [];
  }

  async preflight(): Promise<string[]> {
    if (this.bridgeActive) {
      try { return await this.bridge.preflight(); } catch { this.bridgeActive = false; }
    }
    return this.validate();
  }

  findNodeById(id: string, root?: WidgetNode): WidgetNode | null {
    const searchRoot = root ?? this.getDocument()?.root;
    if (!searchRoot) return null;
    if (searchRoot.id === id) return searchRoot;
    for (const child of searchRoot.children) {
      const found = this.findNodeById(id, child);
      if (found) return found;
    }
    return null;
  }

  addNode(parentId: string, nodeData: Partial<WidgetNode>): string {
    const widgetClass = nodeData.class ?? 'CanvasPanel';
    const identity = this.allocateNodeIdentity(widgetClass);
    const node: WidgetNode = {
      id: identity.id,
      class: widgetClass,
      name: nodeData.name || identity.name,
      props: nodeData.props ?? {},
      slot: nodeData.slot ?? {},
      children: [],
      interactions: []
    };
    void this.execute({ type: 'createNode', parentId, node });
    return identity.id;
  }

  private allocateNodeIdentity(widgetClass: WidgetClass): { id: string; name: string } {
    const ids = new Set<string>();
    const names = new Set<string>();
    const visit = (node: WidgetNode): void => {
      ids.add(node.id);
      names.add(node.name);
      node.children.forEach(visit);
    };
    const document = this.getDocument();
    if (document) visit(document.root);
    let id: string;
    do id = `node_${this.nextNodeNumber++}`; while (ids.has(id));
    let name: string;
    let nextName = this.nextNameNumber.get(widgetClass) ?? 1;
    do name = `${widgetClass}_${nextName++}`; while (names.has(name));
    this.nextNameNumber.set(widgetClass, nextName);
    return { id, name };
  }

  private seedIdentityCounters(root: WidgetNode): void {
    this.nextNodeNumber = 1;
    this.nextNameNumber.clear();
    const visit = (node: WidgetNode): void => {
      const idMatch = /^node_(\d+)$/.exec(node.id);
      if (idMatch) this.nextNodeNumber = Math.max(this.nextNodeNumber, Number(idMatch[1]) + 1);
      const nameMatch = /^(CanvasPanel|HorizontalBox|VerticalBox|Overlay|SizeBox|Border|Image|TextBlock|Button|WidgetSwitcher)_(\d+)$/.exec(node.name);
      if (nameMatch) {
        const widgetClass = nameMatch[1] as WidgetClass;
        this.nextNameNumber.set(widgetClass, Math.max(this.nextNameNumber.get(widgetClass) ?? 1, Number(nameMatch[2]) + 1));
      }
      node.children.forEach(visit);
    };
    visit(root);
  }

  updateNode(nodeId: string, updates: Partial<WidgetNode>): void {
    const patch = {
      ...(updates.name === undefined ? {} : { name: updates.name }),
      ...(updates.class === undefined ? {} : { class: updates.class }),
      ...(updates.slot === undefined ? {} : { slot: updates.slot }),
      ...(updates.props === undefined ? {} : { props: updates.props })
    };
    void this.execute({ type: 'updateNode', nodeId, patch });
  }

  deleteNode(nodeId: string): void {
    void this.execute({ type: 'deleteNode', nodeId });
  }

  setInteractions(nodeId: string, interactions: Interaction[]): void {
    void this.execute({ type: 'setInteractions', nodeId, interactions });
  }

  getInteractions(nodeId: string): Interaction[] {
    return this.findNodeById(nodeId)?.interactions ?? [];
  }

  async undo(): Promise<void> {
    if (!this.core || !this.canUndo()) return;
    const revision = this.getRevision();
    this.core.undo(revision);
    this.undoDepth--;
    this.redoDepth++;
    this.notify();
    if (this.bridgeActive) {
      try { this.reconcileRemote(await this.bridge.undo(revision)); }
      catch { this.bridgeActive = false; }
    }
  }

  async redo(): Promise<void> {
    if (!this.core || !this.canRedo()) return;
    const revision = this.getRevision();
    this.core.redo(revision);
    this.undoDepth++;
    this.redoDepth--;
    this.notify();
    if (this.bridgeActive) {
      try { this.reconcileRemote(await this.bridge.redo(revision)); }
      catch { this.bridgeActive = false; }
    }
  }

  canUndo(): boolean { return this.undoDepth > 0; }
  canRedo(): boolean { return this.redoDepth > 0; }

  subscribe(listener: () => void): () => void {
    this.listeners.push(listener);
    return () => { this.listeners = this.listeners.filter(item => item !== listener); };
  }

  private async execute(command: Command): Promise<void> {
    if (!this.core) return;
    const revision = this.getRevision();
    this.core.execute(command, { expectedRevision: revision });
    this.undoDepth++;
    this.redoDepth = 0;
    this.notify();
    if (this.bridgeActive) {
      try {
        this.reconcileRemote(await this.bridge.execute([command], revision));
      } catch {
        this.bridgeActive = false;
      }
    }
  }

  private reconcileRemote(document: UMGForgeDocument | null): void {
    if (!document || !this.core) return;
    if (serializeDocument(document) === serializeDocument(this.core.getDocument())) return;
    this.replaceCore(document);
    this.undoDepth = document.revision > 0 ? 1 : 0;
    this.redoDepth = 0;
    this.notify();
  }

  private replaceCore(document: UMGForgeDocument): void {
    validateDocument(document);
    this.core = new EditorCore(document);
    this.seedIdentityCounters(document.root);
  }

  private async pollBridge(): Promise<void> {
    if (this.polling || !this.core) return;
    this.polling = true;
    try {
      if (!this.bridgeActive) this.bridgeActive = await this.bridge.available();
      if (!this.bridgeActive) return;
      const remote = await this.bridge.document();
      if (remote && serializeDocument(remote) !== serializeDocument(this.core.getDocument())) {
        this.replaceCore(remote);
        this.undoDepth = remote.revision > 0 ? 1 : 0;
        this.redoDepth = 0;
        this.notify();
      }
    } catch {
      this.bridgeActive = false;
    } finally {
      this.polling = false;
    }
  }

  private notify(): void { this.listeners.forEach(listener => listener()); }
}

export function createExampleDocument(): UMGForgeDocument {
  const node = (
    id: string,
    widgetClass: WidgetClass,
    name: string,
    props: Record<string, unknown> = {},
    slot: Record<string, unknown> | null = {},
    children: WidgetNode[] = []
  ): WidgetNode => ({ id, class: widgetClass, name, props, slot, children, interactions: [] });

  return {
    version: DOCUMENT_VERSION,
    revision: 0,
    root: node('root', 'CanvasPanel', 'RootCanvas', {}, {}, [
      node('overlay_main', 'Overlay', 'MainOverlay', {}, {
        anchors: { min: [0, 0], max: [1, 1] },
        offsets: { left: 0, top: 0, right: 0, bottom: 0 }
      }, [
        node('bg_image', 'Image', 'Background', { color: '#1a1a1a' }),
        node('vbox_content', 'VerticalBox', 'ContentBox', {}, {}, [
          node('text_title', 'TextBlock', 'Title', {
            text: 'Welcome to UMGForge', fontSize: 32, color: '#e0e0e0'
          }, { horizontalAlignment: 'Center', padding: { left: 0, top: 20, right: 0, bottom: 20 } }),
          node('hbox_buttons', 'HorizontalBox', 'ButtonRow', {}, {
            horizontalAlignment: 'Center', padding: { left: 0, top: 10, right: 0, bottom: 10 }
          }, [
            node('btn_start', 'Button', 'StartButton', { backgroundColor: '#0066cc', text: 'Start' }, {
              padding: { left: 5, top: 0, right: 5, bottom: 0 }
            }),
            node('btn_settings', 'Button', 'SettingsButton', { backgroundColor: '#666666', text: 'Settings' }, {
              padding: { left: 5, top: 0, right: 5, bottom: 0 }
            })
          ])
        ])
      ])
    ])
  };
}
