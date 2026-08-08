import { z } from "zod";

export const widgetClasses = [
  "CanvasPanel", "HorizontalBox", "VerticalBox", "Overlay", "SizeBox", "Border",
  "Image", "TextBlock", "Button", "WidgetSwitcher"
] as const;

export const visibilitySchema = z.enum(["Visible", "Collapsed", "Hidden", "HitTestInvisible", "SelfHitTestInvisible"]);
const jsonObjectSchema = z.record(z.string(), z.unknown());

export const interactionSchema = z.discriminatedUnion("type", [
  z.object({ type: z.literal("SetSwitcherIndex"), targetId: z.string().min(1), index: z.number().int().nonnegative() }).strict(),
  z.object({ type: z.literal("SetVisibility"), targetId: z.string().min(1), visibility: visibilitySchema }).strict(),
  z.object({ type: z.literal("EmitEvent"), event: z.string().min(1), payload: jsonObjectSchema.optional() }).strict()
]);

interface WidgetInput {
  id: string;
  name: string;
  class: typeof widgetClasses[number];
  slot: Record<string, unknown> | null;
  props: Record<string, unknown>;
  children: WidgetInput[];
  interactions: z.infer<typeof interactionSchema>[];
}

export const widgetNodeSchema: z.ZodType<WidgetInput> = z.lazy(() => z.object({
  id: z.string().min(1),
  name: z.string().min(1),
  class: z.enum(widgetClasses),
  slot: jsonObjectSchema.nullable(),
  props: jsonObjectSchema,
  children: z.array(widgetNodeSchema),
  interactions: z.array(interactionSchema)
}).strict());

export const documentSchema = z.object({
  version: z.literal("1.0"),
  revision: z.number().int().nonnegative(),
  root: widgetNodeSchema
}).strict();

const revisionSchema = z.object({ expectedRevision: z.number().int().nonnegative() }).strict();
const dryRevisionShape = { expectedRevision: z.number().int().nonnegative(), dryRun: z.boolean().optional() } as const;

export const schemas = {
  get_document: z.object({}).strict(),
  get_selection: z.object({}).strict(),
  create_widget: z.object({
    ...dryRevisionShape,
    parentId: z.string().min(1),
    index: z.number().int().nonnegative().optional(),
    id: z.string().min(1).optional(),
    name: z.string().min(1),
    class: z.enum(widgetClasses),
    slot: jsonObjectSchema.nullable().optional(),
    props: jsonObjectSchema.optional(),
    interactions: z.array(interactionSchema).optional()
  }).strict(),
  update_widget: z.object({
    ...dryRevisionShape,
    nodeId: z.string().min(1),
    patch: z.object({
      name: z.string().min(1).optional(),
      class: z.enum(widgetClasses).optional(),
      slot: jsonObjectSchema.nullable().optional(),
      props: jsonObjectSchema.optional()
    }).strict()
  }).strict(),
  move_widget: z.object({ ...dryRevisionShape, nodeId: z.string().min(1), parentId: z.string().min(1), index: z.number().int().nonnegative().optional() }).strict(),
  delete_widget: z.object({ ...dryRevisionShape, nodeId: z.string().min(1) }).strict(),
  set_interactions: z.object({ ...dryRevisionShape, nodeId: z.string().min(1), interactions: z.array(interactionSchema) }).strict(),
  select: z.object({ expectedRevision: z.number().int().nonnegative(), nodeIds: z.array(z.string().min(1)) }).strict(),
  validate: z.object({}).strict(),
  undo: revisionSchema,
  redo: revisionSchema,
  save_document: z.object({ expectedRevision: z.number().int().nonnegative(), path: z.string().min(1) }).strict(),
  load_document: z.object({ expectedRevision: z.number().int().nonnegative(), path: z.string().min(1) }).strict()
} as const;

export type OperationName = keyof typeof schemas;

export const commandRequestSchema = z.object({
  operation: z.enum(Object.keys(schemas) as [OperationName, ...OperationName[]]),
  input: z.unknown()
}).strict();
