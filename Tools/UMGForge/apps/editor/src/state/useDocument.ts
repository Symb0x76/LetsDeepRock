import { useState, useEffect, useCallback } from 'react';
import { DocumentAdapter, createExampleDocument } from '../core-adapter';
import type { DocumentData, NodeData, WidgetClass } from '../core-adapter';

export function useDocument() {
  const [adapter] = useState(() => new DocumentAdapter());
  const [document, setDocument] = useState<DocumentData | null>(null);
  const [selectedNodeId, setSelectedNodeId] = useState<string | null>(null);

  useEffect(() => {
    adapter.loadDocument(createExampleDocument());
    setDocument(adapter.getDocument());
    const unsubscribe = adapter.subscribe(() => {
      setDocument(adapter.getDocument());
    });
    return () => {
      unsubscribe();
      adapter.dispose();
    };
  }, [adapter]);

  const selectNode = useCallback((id: string | null) => {
    setSelectedNodeId(id);
  }, []);

  const addNode = useCallback((parentId: string, type: WidgetClass, name?: string) => {
    const newId = adapter.addNode(parentId, { class: type, ...(name ? { name } : {}) });
    setSelectedNodeId(newId);
  }, [adapter]);

  const updateNode = useCallback((nodeId: string, updates: Partial<NodeData>) => {
    adapter.updateNode(nodeId, updates);
  }, [adapter]);

  const deleteNode = useCallback((nodeId: string) => {
    if (selectedNodeId === nodeId) {
      setSelectedNodeId(null);
    }
    adapter.deleteNode(nodeId);
  }, [adapter, selectedNodeId]);

  const undo = useCallback(async () => {
    await adapter.undo();
  }, [adapter]);

  const redo = useCallback(async () => {
    await adapter.redo();
  }, [adapter]);

  const saveToFile = useCallback(() => {
    const json = adapter.exportJSON();
    const blob = new Blob([json], { type: 'application/json' });
    const url = URL.createObjectURL(blob);
    const a = window.document.createElement('a');
    a.href = url;
    a.download = `${document?.root.name || 'document'}.json`;
    a.click();
    URL.revokeObjectURL(url);
  }, [adapter, document]);

  const loadFromFile = useCallback(() => {
    const input = window.document.createElement('input');
    input.type = 'file';
    input.accept = '.json';
    input.onchange = (e: Event) => {
      const file = (e.target as HTMLInputElement).files?.[0];
      if (!file) return;
      const reader = new FileReader();
      reader.onload = (evt) => {
        const json = evt.target?.result as string;
        if (adapter.importJSON(json)) {
          setSelectedNodeId(null);
        } else {
          alert('加载失败：无效的 JSON 格式');
        }
      };
      reader.readAsText(file);
    };
    input.click();
  }, [adapter]);

  const getSelectedNode = useCallback(() => {
    if (!selectedNodeId) return null;
    return adapter.findNodeById(selectedNodeId);
  }, [adapter, selectedNodeId]);

  return {
    document,
    selectedNodeId,
    selectedNode: getSelectedNode(),
    selectNode,
    addNode,
    updateNode,
    deleteNode,
    undo,
    redo,
    canUndo: adapter.canUndo(),
    canRedo: adapter.canRedo(),
    saveToFile,
    loadFromFile,
    adapter
  };
}
