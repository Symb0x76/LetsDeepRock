import React from 'react';
import type { NodeData } from '../core-adapter';
import './Hierarchy.css';

interface HierarchyProps {
  root: NodeData | null;
  selectedId: string | null;
  onSelect: (id: string) => void;
  onDelete: (id: string) => void;
}

export function Hierarchy({ root, selectedId, onSelect, onDelete }: HierarchyProps) {
  if (!root) return <div className="hierarchy">加载中...</div>;

  return (
    <div className="hierarchy">
      <div className="hierarchy-header">层级</div>
      <div className="hierarchy-tree">
        <TreeNode
          node={root}
          selectedId={selectedId}
          onSelect={onSelect}
          onDelete={onDelete}
          depth={0}
        />
      </div>
    </div>
  );
}

interface TreeNodeProps {
  node: NodeData;
  selectedId: string | null;
  onSelect: (id: string) => void;
  onDelete: (id: string) => void;
  depth: number;
}

function TreeNode({ node, selectedId, onSelect, onDelete, depth }: TreeNodeProps) {
  const [expanded, setExpanded] = React.useState(true);
  const isSelected = node.id === selectedId;
  const hasChildren = node.children.length > 0;

  return (
    <div className="tree-node">
      <div
        className={`tree-node-content ${isSelected ? 'selected' : ''}`}
        style={{ paddingLeft: `${depth * 16 + 4}px` }}
        onClick={() => onSelect(node.id)}
      >
        {hasChildren && (
          <button
            className="tree-node-toggle"
            onClick={(e) => {
              e.stopPropagation();
              setExpanded(!expanded);
            }}
          >
            {expanded ? '▼' : '▶'}
          </button>
        )}
        <span className="tree-node-icon">{getNodeIcon(node.class)}</span>
        <span className="tree-node-name">{node.name}</span>
        <span className="tree-node-type">({node.class})</span>
        {node.id !== 'root' && (
          <button
            className="tree-node-delete button-small"
            onClick={(e) => {
              e.stopPropagation();
              onDelete(node.id);
            }}
            title="删除"
          >
            ×
          </button>
        )}
      </div>
      {expanded && hasChildren && (
        <div className="tree-node-children">
          {node.children.map(child => (
            <TreeNode
              key={child.id}
              node={child}
              selectedId={selectedId}
              onSelect={onSelect}
              onDelete={onDelete}
              depth={depth + 1}
            />
          ))}
        </div>
      )}
    </div>
  );
}

function getNodeIcon(type: string): string {
  switch (type) {
    case 'CanvasPanel': return '□';
    case 'HorizontalBox': return '⬌';
    case 'VerticalBox': return '⬍';
    case 'Overlay': return '◫';
    case 'Button': return '▭';
    case 'TextBlock': return 'T';
    case 'Image': return '🖼';
    case 'WidgetSwitcher': return '⇄';
    default: return '○';
  }
}
