import './Toolbar.css';
import type { WidgetClass } from '../core-adapter';

interface ToolbarProps {
  onAddNode: (type: WidgetClass) => void;
  onSave: () => void;
  onLoad: () => void;
  onUndo: () => void;
  onRedo: () => void;
  canUndo: boolean;
  canRedo: boolean;
}

const WIDGET_TYPES = [
  { type: 'CanvasPanel', label: 'Canvas' },
  { type: 'HorizontalBox', label: 'HBox' },
  { type: 'VerticalBox', label: 'VBox' },
  { type: 'Overlay', label: 'Overlay' },
  { type: 'Button', label: 'Button' },
  { type: 'TextBlock', label: 'Text' },
  { type: 'Image', label: 'Image' },
  { type: 'WidgetSwitcher', label: 'Switcher' }
] satisfies Array<{ type: WidgetClass; label: string }>;

export function Toolbar({ onAddNode, onSave, onLoad, onUndo, onRedo, canUndo, canRedo }: ToolbarProps) {
  return (
    <div className="toolbar">
      <div className="toolbar-section">
        <button onClick={onLoad} className="button-small">打开</button>
        <button onClick={onSave} className="button-small button-primary">保存</button>
      </div>

      <div className="toolbar-separator" />

      <div className="toolbar-section">
        <button onClick={onUndo} disabled={!canUndo} className="button-small" title="撤销">↶</button>
        <button onClick={onRedo} disabled={!canRedo} className="button-small" title="重做">↷</button>
      </div>

      <div className="toolbar-separator" />

      <div className="toolbar-section toolbar-widgets">
        <span className="toolbar-label">添加控件:</span>
        {WIDGET_TYPES.map(({ type, label }) => (
          <button
            key={type}
            onClick={() => onAddNode(type)}
            className="button-small"
          >
            {label}
          </button>
        ))}
      </div>
    </div>
  );
}
