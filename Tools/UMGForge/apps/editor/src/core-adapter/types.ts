import type {
  Command,
  Interaction,
  UMGForgeDocument,
  WidgetClass,
  WidgetNode
} from '@umgforge/core';

export type DocumentData = UMGForgeDocument;
export type NodeData = WidgetNode;
export type NodeType = WidgetClass;
export type EventAction = Interaction;
export type { Command, Interaction, UMGForgeDocument, WidgetClass, WidgetNode };

export interface SlotConfig {
  anchors?: { min: [number, number]; max: [number, number] };
  offsets?: { left: number; top: number; right: number; bottom: number };
  alignment?: [number, number];
  position?: [number, number];
  size?: [number, number];
  zOrder?: number;
  horizontalAlignment?: 'Fill' | 'Left' | 'Center' | 'Right';
  verticalAlignment?: 'Fill' | 'Top' | 'Center' | 'Bottom';
  padding?: { left: number; top: number; right: number; bottom: number };
  fillWidth?: number;
  fillHeight?: number;
}
