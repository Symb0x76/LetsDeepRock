import React, { useRef, useState } from 'react';
import type { NodeData } from '../../core-adapter';
import { CanvasRenderer } from './CanvasRenderer';
import './Canvas.css';

interface CanvasProps {
  root: NodeData | null;
  canvasSize: [number, number];
  selectedId: string | null;
  onSelect: (id: string | null) => void;
  onUpdateNode: (id: string, updates: Partial<NodeData>) => void;
}

export function Canvas({ root, canvasSize, selectedId, onSelect, onUpdateNode }: CanvasProps) {
  const containerRef = useRef<HTMLDivElement>(null);
  const [scale, setScale] = useState(0.5);
  const [pan, setPan] = useState({ x: 0, y: 0 });
  const [isPanning, setIsPanning] = useState(false);
  const [panStart, setPanStart] = useState({ x: 0, y: 0 });

  const handleWheel = (e: React.WheelEvent) => {
    if (e.ctrlKey || e.metaKey) {
      e.preventDefault();
      const delta = -e.deltaY * 0.001;
      setScale(prev => Math.max(0.1, Math.min(2, prev + delta)));
    }
  };

  const handleMouseDown = (e: React.MouseEvent) => {
    if (e.button === 1 || (e.button === 0 && e.altKey)) {
      e.preventDefault();
      setIsPanning(true);
      setPanStart({ x: e.clientX - pan.x, y: e.clientY - pan.y });
    }
  };

  const handleMouseMove = (e: React.MouseEvent) => {
    if (isPanning) {
      setPan({ x: e.clientX - panStart.x, y: e.clientY - panStart.y });
    }
  };

  const handleMouseUp = () => {
    setIsPanning(false);
  };

  if (!root) return <div className="canvas-container">加载中...</div>;

  return (
    <div
      ref={containerRef}
      className="canvas-container"
      onWheel={handleWheel}
      onMouseDown={handleMouseDown}
      onMouseMove={handleMouseMove}
      onMouseUp={handleMouseUp}
      onMouseLeave={handleMouseUp}
    >
      <div className="canvas-controls">
        <button onClick={() => setScale(0.25)} className="button-small">25%</button>
        <button onClick={() => setScale(0.5)} className="button-small">50%</button>
        <button onClick={() => setScale(1)} className="button-small">100%</button>
        <span className="canvas-scale-label">{Math.round(scale * 100)}%</span>
      </div>

      <div
        className="canvas-viewport"
        style={{
          transform: `translate(${pan.x}px, ${pan.y}px)`,
          cursor: isPanning ? 'grabbing' : 'default'
        }}
      >
        <div
          className="canvas-surface"
          style={{
            width: canvasSize[0],
            height: canvasSize[1],
            transform: `scale(${scale})`,
            transformOrigin: '0 0'
          }}
        >
          <CanvasRenderer
            node={root}
            selectedId={selectedId}
            onSelect={onSelect}
            onUpdateNode={onUpdateNode}
          />
        </div>
      </div>
    </div>
  );
}
