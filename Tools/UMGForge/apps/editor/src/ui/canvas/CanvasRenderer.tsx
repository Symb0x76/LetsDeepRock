import React from 'react';
import type { NodeData, SlotConfig } from '../../core-adapter';
import './CanvasRenderer.css';

interface CanvasRendererProps {
  node: NodeData;
  selectedId: string | null;
  onSelect: (id: string | null) => void;
  onUpdateNode: (id: string, updates: Partial<NodeData>) => void;
}

export function CanvasRenderer({ node, selectedId, onSelect, onUpdateNode }: CanvasRendererProps) {
  return (
    <RenderedNode
      node={node}
      selectedId={selectedId}
      onSelect={onSelect}
      onUpdateNode={onUpdateNode}
    />
  );
}

interface RenderedNodeProps {
  node: NodeData;
  selectedId: string | null;
  onSelect: (id: string | null) => void;
  onUpdateNode: (id: string, updates: Partial<NodeData>) => void;
}

function RenderedNode({ node, selectedId, onSelect, onUpdateNode }: RenderedNodeProps) {
  const isSelected = node.id === selectedId;
  const style = getNodeStyle(node);
  const layoutStyle = getLayoutStyle(node);

  const handleClick = (e: React.MouseEvent) => {
    e.stopPropagation();
    onSelect(node.id);
  };

  const containerStyle: React.CSSProperties = {
    ...style,
    ...layoutStyle,
    position: node.class === 'CanvasPanel' && node.id === 'root' ? 'relative' : layoutStyle.position
  };

  return (
    <div
      className={`rendered-node ${isSelected ? 'selected' : ''} type-${node.class.toLowerCase()}`}
      style={containerStyle}
      onClick={handleClick}
      data-node-id={node.id}
      data-node-type={node.class}
    >
      {renderNodeContent(node)}
      {node.children.length > 0 && (
        node.children.map(child => (
          <RenderedNode
            key={child.id}
            node={child}
            selectedId={selectedId}
            onSelect={onSelect}
            onUpdateNode={onUpdateNode}
          />
        ))
      )}
      {isSelected && <div className="selection-outline" />}
    </div>
  );
}

function getLayoutStyle(node: NodeData): React.CSSProperties {
  const type = node.class;
  const slot = (node.slot ?? {}) as SlotConfig;

  if (type === 'CanvasPanel' && node.id === 'root') {
    return { width: '100%', height: '100%', position: 'relative' };
  }

  if (slot.position && slot.size) {
    return {
      position: 'absolute',
      left: slot.position[0],
      top: slot.position[1],
      width: slot.size[0],
      height: slot.size[1],
      zIndex: slot.zOrder || 0
    };
  }

  if (slot.anchors && slot.offsets) {
    const { min, max } = slot.anchors;
    const { left, top, right, bottom } = slot.offsets;
    return {
      position: 'absolute',
      left: `${min[0] * 100}%`,
      top: `${min[1] * 100}%`,
      right: `${(1 - max[0]) * 100}%`,
      bottom: `${(1 - max[1]) * 100}%`,
      marginLeft: left,
      marginTop: top,
      marginRight: -right,
      marginBottom: -bottom
    };
  }

  const baseStyle: React.CSSProperties = {};

  if (slot.padding) {
    const { left, top, right, bottom } = slot.padding;
    baseStyle.padding = `${top}px ${right}px ${bottom}px ${left}px`;
  }

  return baseStyle;
}

function getNodeStyle(node: NodeData): React.CSSProperties {
  const type = node.class;
  const properties = node.props;
  const style: React.CSSProperties = {};

  if (type === 'CanvasPanel') {
    style.display = 'block';
    style.position = 'relative';
  } else if (type === 'HorizontalBox') {
    style.display = 'flex';
    style.flexDirection = 'row';
    style.gap = '0';
  } else if (type === 'VerticalBox') {
    style.display = 'flex';
    style.flexDirection = 'column';
    style.gap = '0';
  } else if (type === 'Overlay') {
    style.display = 'grid';
    style.gridTemplateColumns = '1fr';
    style.gridTemplateRows = '1fr';
  } else if (type === 'Button') {
    style.background = properties.backgroundColor as string || '#4a4a4a';
    style.border = '1px solid #606060';
    style.borderRadius = '4px';
    style.padding = '8px 16px';
    style.cursor = 'pointer';
    style.color = properties.color as string || '#ebebeb';
  } else if (type === 'TextBlock') {
    style.color = properties.color as string || '#ebebeb';
    style.fontSize = properties.fontSize ? `${properties.fontSize}px` : '14px';
  } else if (type === 'Image') {
    style.background = properties.color as string || '#4a4a4a';
    style.minWidth = '64px';
    style.minHeight = '64px';
  }

  return style;
}

function renderNodeContent(node: NodeData): React.ReactNode {
  const type = node.class;
  const properties = node.props;

  if (type === 'Button') {
    return (properties.text as string) || 'Button';
  } else if (type === 'TextBlock') {
    return (properties.text as string) || 'Text';
  } else if (type === 'Image') {
    return null;
  }

  return null;
}
