import type { DocumentAdapter, EventAction, NodeData, SlotConfig } from '../../core-adapter';
import './Inspector.css';

interface InspectorProps {
  selectedNode: NodeData | null;
  onUpdate: (nodeId: string, updates: Partial<NodeData>) => void;
  adapter: DocumentAdapter;
}

export function Inspector({ selectedNode, onUpdate, adapter }: InspectorProps) {
  if (!selectedNode) {
    return (
      <div className="inspector">
        <div className="inspector-header">属性</div>
        <div className="inspector-content">
          <div className="inspector-empty">未选择节点</div>
        </div>
      </div>
    );
  }

  return (
    <div className="inspector">
      <div className="inspector-header">属性</div>
      <div className="inspector-content">
        <BasicSection node={selectedNode} onUpdate={onUpdate} />
        <SlotSection node={selectedNode} onUpdate={onUpdate} />
        <AppearanceSection node={selectedNode} onUpdate={onUpdate} />
        {selectedNode.class === 'Button' && (
          <InteractionSection node={selectedNode} adapter={adapter} />
        )}
      </div>
    </div>
  );
}

function BasicSection({ node, onUpdate }: { node: NodeData; onUpdate: any }) {
  return (
    <div className="inspector-section">
      <div className="inspector-section-title">基本信息</div>
      <div className="inspector-field">
        <label className="inspector-field-label">名称</label>
        <input
          type="text"
          value={node.name}
          onChange={(e) => onUpdate(node.id, { name: e.target.value })}
        />
      </div>
      <div className="inspector-field">
        <label className="inspector-field-label">类型</label>
        <input type="text" value={node.class} disabled />
      </div>
    </div>
  );
}

function SlotSection({ node, onUpdate }: { node: NodeData; onUpdate: any }) {
  const slot = (node.slot ?? {}) as SlotConfig;

  if (slot.position !== undefined && slot.size !== undefined) {
    return (
      <div className="inspector-section">
        <div className="inspector-section-title">Canvas 插槽</div>
        <div className="inspector-field">
          <label className="inspector-field-label">位置 (X, Y)</label>
          <div className="inspector-field-row">
            <input
              type="number"
              value={slot.position[0]}
              onChange={(e) =>
                onUpdate(node.id, {
                  slot: { ...slot, position: [+e.target.value, slot.position![1]] }
                })
              }
            />
            <input
              type="number"
              value={slot.position[1]}
              onChange={(e) =>
                onUpdate(node.id, {
                  slot: { ...slot, position: [slot.position![0], +e.target.value] }
                })
              }
            />
          </div>
        </div>
        <div className="inspector-field">
          <label className="inspector-field-label">尺寸 (W, H)</label>
          <div className="inspector-field-row">
            <input
              type="number"
              value={slot.size[0]}
              onChange={(e) =>
                onUpdate(node.id, {
                  slot: { ...slot, size: [+e.target.value, slot.size![1]] }
                })
              }
            />
            <input
              type="number"
              value={slot.size[1]}
              onChange={(e) =>
                onUpdate(node.id, {
                  slot: { ...slot, size: [slot.size![0], +e.target.value] }
                })
              }
            />
          </div>
        </div>
      </div>
    );
  }

  return (
    <div className="inspector-section">
      <div className="inspector-section-title">布局插槽</div>
      {slot.horizontalAlignment !== undefined && (
        <div className="inspector-field">
          <label className="inspector-field-label">水平对齐</label>
          <select
            value={slot.horizontalAlignment}
            onChange={(e) =>
              onUpdate(node.id, { slot: { ...slot, horizontalAlignment: e.target.value as any } })
            }
          >
            <option value="Fill">Fill</option>
            <option value="Left">Left</option>
            <option value="Center">Center</option>
            <option value="Right">Right</option>
          </select>
        </div>
      )}
      {slot.padding !== undefined && (
        <div className="inspector-field">
          <label className="inspector-field-label">内边距 (L T R B)</label>
          <div className="inspector-field-row">
            <input
              type="number"
              value={slot.padding.left}
              onChange={(e) =>
                onUpdate(node.id, {
                  slot: { ...slot, padding: { ...slot.padding!, left: +e.target.value } }
                })
              }
            />
            <input
              type="number"
              value={slot.padding.top}
              onChange={(e) =>
                onUpdate(node.id, {
                  slot: { ...slot, padding: { ...slot.padding!, top: +e.target.value } }
                })
              }
            />
            <input
              type="number"
              value={slot.padding.right}
              onChange={(e) =>
                onUpdate(node.id, {
                  slot: { ...slot, padding: { ...slot.padding!, right: +e.target.value } }
                })
              }
            />
            <input
              type="number"
              value={slot.padding.bottom}
              onChange={(e) =>
                onUpdate(node.id, {
                  slot: { ...slot, padding: { ...slot.padding!, bottom: +e.target.value } }
                })
              }
            />
          </div>
        </div>
      )}
    </div>
  );
}

function AppearanceSection({ node, onUpdate }: { node: NodeData; onUpdate: any }) {
  const type = node.class;
  const properties = node.props;

  if (type === 'TextBlock') {
    return (
      <div className="inspector-section">
        <div className="inspector-section-title">外观</div>
        <div className="inspector-field">
          <label className="inspector-field-label">文本内容</label>
          <input
            type="text"
            value={(properties.text as string) || ''}
            onChange={(e) =>
              onUpdate(node.id, { props: { ...properties, text: e.target.value } })
            }
          />
        </div>
        <div className="inspector-field">
          <label className="inspector-field-label">字体大小</label>
          <input
            type="number"
            value={(properties.fontSize as number) || 14}
            onChange={(e) =>
              onUpdate(node.id, { props: { ...properties, fontSize: +e.target.value } })
            }
          />
        </div>
        <div className="inspector-field">
          <label className="inspector-field-label">颜色</label>
          <input
            type="color"
            value={(properties.color as string) || '#e0e0e0'}
            onChange={(e) =>
              onUpdate(node.id, { props: { ...properties, color: e.target.value } })
            }
          />
        </div>
      </div>
    );
  }

  if (type === 'Button') {
    return (
      <div className="inspector-section">
        <div className="inspector-section-title">外观</div>
        <div className="inspector-field">
          <label className="inspector-field-label">按钮文本</label>
          <input
            type="text"
            value={(properties.text as string) || ''}
            onChange={(e) =>
              onUpdate(node.id, { props: { ...properties, text: e.target.value } })
            }
          />
        </div>
        <div className="inspector-field">
          <label className="inspector-field-label">背景颜色</label>
          <input
            type="color"
            value={(properties.backgroundColor as string) || '#666666'}
            onChange={(e) =>
              onUpdate(node.id, { props: { ...properties, backgroundColor: e.target.value } })
            }
          />
        </div>
      </div>
    );
  }

  if (type === 'Image') {
    return (
      <div className="inspector-section">
        <div className="inspector-section-title">外观</div>
        <div className="inspector-field">
          <label className="inspector-field-label">颜色</label>
          <input
            type="color"
            value={(properties.color as string) || '#666666'}
            onChange={(e) =>
              onUpdate(node.id, { props: { ...properties, color: e.target.value } })
            }
          />
        </div>
      </div>
    );
  }

  return null;
}

function InteractionSection({ node, adapter }: { node: NodeData; adapter: DocumentAdapter }) {
  const actions = node.interactions;

  const addAction = (actionType: EventAction['type']) => {
    const findSwitcher = (candidate: NodeData): NodeData | null => {
      if (candidate.class === 'WidgetSwitcher' && candidate.children.length > 0) return candidate;
      for (const child of candidate.children) {
        const found = findSwitcher(child);
        if (found) return found;
      }
      return null;
    };
    const switcher = findSwitcher(adapter.getDocument()!.root);
    if (actionType === 'SetSwitcherIndex' && !switcher) return;
    const newAction: EventAction =
      actionType === 'SetSwitcherIndex'
        ? { type: 'SetSwitcherIndex', targetId: switcher!.id, index: 0 }
        : actionType === 'SetVisibility'
        ? { type: 'SetVisibility', targetId: node.id, visibility: 'Hidden' }
        : { type: 'EmitEvent', event: 'NewEvent' };

    adapter.setInteractions(node.id, [...actions, newAction]);
  };

  const updateAction = (index: number, updates: Record<string, unknown>) => {
    const newActions = [...actions];
    newActions[index] = { ...newActions[index], ...updates } as EventAction;
    adapter.setInteractions(node.id, newActions);
  };

  const removeAction = (index: number) => {
    adapter.setInteractions(node.id, actions.filter((_, i) => i !== index));
  };

  return (
    <div className="inspector-section">
      <div className="inspector-section-title">交互事件</div>
      <div style={{ marginBottom: 'var(--gap-sm)', color: 'var(--text-muted)', fontSize: '12px' }}>
        OnClicked
      </div>
      {actions.map((action, index) => (
        <div key={index} className="event-action">
          <div className="event-action-header">
            <span className="event-action-type">{action.type}</span>
            <button
              className="event-action-remove button-small"
              onClick={() => removeAction(index)}
            >
              ×
            </button>
          </div>
          {action.type === 'SetSwitcherIndex' && (
            <>
              <div className="inspector-field">
                <label className="inspector-field-label">目标 ID</label>
                <input
                  type="text"
                  value={action.targetId}
                  onChange={(e) => updateAction(index, { targetId: e.target.value })}
                />
              </div>
              <div className="inspector-field">
                <label className="inspector-field-label">索引</label>
                <input
                  type="number"
                  value={action.index}
                  onChange={(e) => updateAction(index, { index: +e.target.value })}
                />
              </div>
            </>
          )}
          {action.type === 'SetVisibility' && (
            <>
              <div className="inspector-field">
                <label className="inspector-field-label">目标 ID</label>
                <input
                  type="text"
                  value={action.targetId}
                  onChange={(e) => updateAction(index, { targetId: e.target.value })}
                />
              </div>
              <div className="inspector-field">
                <label className="inspector-field-label">可见性</label>
                <select
                  value={action.visibility}
                  onChange={(e) =>
                    updateAction(index, { visibility: e.target.value as any })
                  }
                >
                  <option value="Visible">Visible</option>
                  <option value="Hidden">Hidden</option>
                  <option value="Collapsed">Collapsed</option>
                </select>
              </div>
            </>
          )}
          {action.type === 'EmitEvent' && (
            <div className="inspector-field">
              <label className="inspector-field-label">事件名称</label>
              <input
                type="text"
                value={action.event}
                onChange={(e) => updateAction(index, { event: e.target.value })}
              />
            </div>
          )}
        </div>
      ))}
      <select
        className="event-add-button"
        value=""
        onChange={(e) => {
          if (e.target.value) {
            addAction(e.target.value as EventAction['type']);
            e.target.value = '';
          }
        }}
      >
        <option value="">+ 添加动作</option>
        <option value="SetSwitcherIndex">SetSwitcherIndex</option>
        <option value="SetVisibility">SetVisibility</option>
        <option value="EmitEvent">EmitEvent</option>
      </select>
    </div>
  );
}
