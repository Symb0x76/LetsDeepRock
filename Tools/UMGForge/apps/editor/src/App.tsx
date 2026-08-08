import { useEffect } from 'react';
import { useDocument } from './state/useDocument';
import { Toolbar } from './ui/Toolbar';
import { Hierarchy } from './ui/Hierarchy';
import { Canvas } from './ui/canvas/Canvas';
import { Inspector } from './ui/inspector/Inspector';
import './App.css';
import type { WidgetClass } from './core-adapter';

function App() {
  const {
    document,
    selectedNodeId,
    selectedNode,
    selectNode,
    addNode,
    updateNode,
    deleteNode,
    undo,
    redo,
    canUndo,
    canRedo,
    saveToFile,
    loadFromFile,
    adapter
  } = useDocument();

  useEffect(() => {
    const handleKeyDown = (e: KeyboardEvent) => {
      if ((e.ctrlKey || e.metaKey) && e.key === 'z' && !e.shiftKey) {
        e.preventDefault();
        undo();
      } else if ((e.ctrlKey || e.metaKey) && (e.key === 'y' || (e.key === 'z' && e.shiftKey))) {
        e.preventDefault();
        redo();
      } else if ((e.ctrlKey || e.metaKey) && e.key === 's') {
        e.preventDefault();
        saveToFile();
      }
    };

    window.addEventListener('keydown', handleKeyDown);
    return () => window.removeEventListener('keydown', handleKeyDown);
  }, [undo, redo, saveToFile]);

  const handleAddNode = (type: WidgetClass) => {
    const parentId = selectedNodeId || 'root';
    addNode(parentId, type);
  };

  const problems = document ? adapter.validate() : [];

  return (
    <div className="app">
      <Toolbar
        onAddNode={handleAddNode}
        onSave={saveToFile}
        onLoad={loadFromFile}
        onUndo={undo}
        onRedo={redo}
        canUndo={canUndo}
        canRedo={canRedo}
      />
      <div className="app-main">
        <Hierarchy
          root={document?.root || null}
          selectedId={selectedNodeId}
          onSelect={selectNode}
          onDelete={deleteNode}
        />
        <Canvas
          root={document?.root || null}
          canvasSize={[1920, 1080]}
          selectedId={selectedNodeId}
          onSelect={selectNode}
          onUpdateNode={updateNode}
        />
        <Inspector selectedNode={selectedNode} onUpdate={updateNode} adapter={adapter} />
      </div>
      <div className="problems">
        <div className="problems-header">
          问题 <span className="problems-count">({problems.length})</span>
        </div>
        <div className="problems-list">
          {problems.length === 0 ? (
            <div className="problem-item">无问题</div>
          ) : (
            problems.map((problem, i) => (
              <div key={i} className="problem-item">
                <span className="problem-icon">⚠</span>
                {problem}
              </div>
            ))
          )}
        </div>
      </div>
    </div>
  );
}

export default App;
