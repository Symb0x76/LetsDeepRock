import { afterEach, describe, expect, it, vi } from 'vitest';
import { serializeDocument, type UMGForgeDocument } from '@umgforge/core';
import { DocumentAdapter, createExampleDocument } from '../index';

const adapters: DocumentAdapter[] = [];
const adapter = (): DocumentAdapter => {
  const value = new DocumentAdapter();
  adapters.push(value);
  value.loadDocument(createExampleDocument());
  return value;
};

afterEach(() => {
  adapters.forEach(value => value.dispose());
  adapters.length = 0;
  vi.restoreAllMocks();
});

describe('DocumentAdapter', () => {
  it('使用 core 正式文档类型并返回快照', () => {
    const value = adapter();
    const first = value.getDocument();
    expect(first?.root.class).toBe('CanvasPanel');
    expect(value.findNodeById('text_title')?.class).toBe('TextBlock');
    expect(value.getDocument()).not.toBe(first);
  });

  it('通过命令写入并推进 revision', () => {
    const value = adapter();
    const id = value.addNode('root', { class: 'Button', name: 'TestBtn' });
    expect(value.findNodeById(id)?.class).toBe('Button');
    expect(value.getRevision()).toBe(1);
  });

  it('扫描文档生成单调且无冲突的 ID 与名称', () => {
    const value = adapter();
    const first = value.addNode('root', { class: 'Button' });
    const second = value.addNode('root', { class: 'Button' });
    const text = value.addNode('root', { class: 'TextBlock' });
    expect([first, second, text]).toEqual(['node_1', 'node_2', 'node_3']);
    expect(value.findNodeById(first)?.name).toBe('Button_1');
    expect(value.findNodeById(second)?.name).toBe('Button_2');
    expect(value.findNodeById(text)?.name).toBe('TextBlock_1');
  });

  it('删除最高编号后仍保持进程内单调编号', () => {
    const value = adapter();
    const first = value.addNode('root', { class: 'Button' });
    value.deleteNode(first);
    const second = value.addNode('root', { class: 'Button' });
    expect(second).toBe('node_2');
    expect(value.findNodeById(second)?.name).toBe('Button_2');
  });

  it('支持 undo 与 redo', async () => {
    const value = adapter();
    const initialChildren = value.getDocument()!.root.children.length;
    value.addNode('root', { class: 'TextBlock', name: 'TestText' });
    await value.undo();
    expect(value.getDocument()!.root.children).toHaveLength(initialChildren);
    await value.redo();
    expect(value.getDocument()!.root.children).toHaveLength(initialChildren + 1);
    expect(value.getRevision()).toBe(3);
  });

  it('把交互写入 node.interactions', () => {
    const value = adapter();
    value.setInteractions('btn_start', [{ type: 'EmitEvent', event: 'StartGame' }]);
    expect(value.findNodeById('btn_start')?.interactions).toEqual([
      { type: 'EmitEvent', event: 'StartGame' }
    ]);
  });

  it('确定性导出 CRLF 并验证导入', () => {
    const value = adapter();
    const json = value.exportJSON();
    expect(json.endsWith('\r\n')).toBe(true);
    expect(json.replace(/\r\n/g, '').includes('\n')).toBe(false);
    expect(value.importJSON(json)).toBe(true);
    expect(value.validate()).toEqual([]);
    expect(value.importJSON('{"version":"bad"}')).toBe(false);
  });

  it('bridge 初次同步以同 revision 的 MCP 文档为准并沿用远端 revision 写入', async () => {
    let remote: UMGForgeDocument = {
      version: '1.0',
      revision: 0,
      root: { id: 'root', name: 'Root', class: 'CanvasPanel', slot: null, props: {}, children: [], interactions: [] }
    };
    const requests: Array<{ operation: string; input: Record<string, unknown> }> = [];
    vi.spyOn(globalThis, 'fetch').mockImplementation(async (input, init) => {
      const url = String(input);
      if (url.endsWith('/status')) return new Response(JSON.stringify({ ok: true }), { status: 200 });
      if (url.endsWith('/document')) return new Response(JSON.stringify({ document: remote }), { status: 200 });
      if (url.endsWith('/commands')) {
        const request = JSON.parse(String(init?.body)) as { operation: string; input: Record<string, unknown> };
        requests.push(request);
        expect(request.input.expectedRevision).toBe(remote.revision);
        if (request.operation === 'create_widget') {
          remote = structuredClone(remote);
          remote.root.children.push({
            id: String(request.input.id),
            name: String(request.input.name),
            class: 'Button',
            slot: request.input.slot as Record<string, unknown>,
            props: request.input.props as Record<string, unknown>,
            children: [],
            interactions: []
          });
          remote.revision++;
        }
        return new Response(JSON.stringify({ document: remote }), { status: 200 });
      }
      return new Response(null, { status: 404 });
    });

    const value = adapter();
    await vi.waitFor(() => expect(serializeDocument(value.getDocument()!)).toBe(serializeDocument(remote)));
    expect(value.findNodeById('overlay_main')).toBeNull();

    const id = value.addNode('root', { class: 'Button', name: 'SharedButton' });
    await vi.waitFor(() => expect(requests).toHaveLength(1));
    await vi.waitFor(() => expect(value.findNodeById(id)?.name).toBe('SharedButton'));
    expect(value.getRevision()).toBe(1);
    expect(remote.revision).toBe(1);
  });
});
