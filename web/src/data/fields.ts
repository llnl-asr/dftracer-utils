import type { TraceSchema } from "./types";

// The record fields the viewer's duql clauses name: dftracer's own, or a path
// record schema's roles and label. An empty name is a field the schema lacks,
// so no clause on it can be written.
export const FIELDS = { pid: "pid", tid: "tid", name: "name", cat: "cat" };

// pid -> entity value, for a path schema whose entity is text (the pid is then
// a hash of it).
const entityByPid = new Map<string, string>();
const entityLabels = new Set<string>();

export function setSchema(schema: TraceSchema | undefined): void {
  if (!schema || schema.decoder !== "path") return;
  FIELDS.pid = schema.fields.entity;
  FIELDS.tid = schema.fields.lane;
  FIELDS.name = schema.fields.label;
  FIELDS.cat = "";
}

export function setEntityLabels(labels: Map<string, string>): void {
  entityByPid.clear();
  entityLabels.clear();
  for (const [pid, label] of labels) {
    entityByPid.set(pid, label);
    entityLabels.add(label);
  }
}

const quote = (s: string) => `"${s.replace(/\\/g, "\\\\").replace(/"/g, '\\"')}"`;

// `field == value` for a text value; null when the schema lacks the field.
export function textClause(field: "name" | "cat", value: string): string | null {
  return FIELDS[field] ? `${FIELDS[field]} == ${quote(value)}` : null;
}

// The clause selecting an entity by its value as the Analyze panel keys it:
// an integer stays a number unless it is a text entity's value.
export function entityClause(value: string): string | null {
  if (!FIELDS.pid) return null;
  const numeric = /^-?\d+$/.test(value) && !entityLabels.has(value);
  return `${FIELDS.pid} == ${numeric ? value : quote(value)}`;
}

// The clause selecting a timeline lane (tid "" for the whole process); null
// when the schema has no entity.
export function laneClause(pid: string, tid: string): string | null {
  if (!FIELDS.pid) return null;
  const label = entityByPid.get(pid);
  const p = `${FIELDS.pid} == ${label != null ? quote(label) : pid}`;
  return tid && FIELDS.tid ? `(${p} and ${FIELDS.tid} == ${tid})` : p;
}
