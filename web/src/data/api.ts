import type {
  CallTreeResponse,
  HistogramResponse,
  InfoResponse,
  ProcTreeNode,
  SelectionStats,
  UntimedResponse,
  VizCounters,
  VizDensityResponse,
  VizQuery,
  VizResponse,
} from "./types";

import { CONFIG } from "./config";

// True when the view is scoped to a single trace file.
export const SINGLE_FILE = CONFIG.file !== "";

function withFile(params: URLSearchParams): URLSearchParams {
  if (CONFIG.file) params.set("file", CONFIG.file);
  return params;
}

// Prefix with the API base (VS Code webview) and append the access token.
export function apiUrl(url: string): string {
  let u = CONFIG.apiBase ? CONFIG.apiBase.replace(/\/$/, "") + url : url;
  if (CONFIG.token)
    u += (u.includes("?") ? "&" : "?") + "token=" + encodeURIComponent(CONFIG.token);
  return u;
}

// Raw request for the in-app API explorer: returns the status and body text
// (never throws on a non-2xx, so the explorer can display error responses).
export async function apiRequest(path: string): Promise<{ status: number; body: string }> {
  try {
    const res = await fetch(apiUrl(path));
    return { status: res.status, body: await res.text() };
  } catch (e) {
    return { status: 0, body: e instanceof Error ? e.message : String(e) };
  }
}

function newRequestId(): string {
  const c = globalThis.crypto;
  if (c && typeof c.randomUUID === "function") return c.randomUUID();
  return `${Date.now().toString(36)}-${Math.random().toString(36).slice(2)}`;
}

export function fetchResolve(
  hashes: string[],
  type: "file" | "host" = "file",
): Promise<{ names: Record<string, string> }> {
  const params = new URLSearchParams({ hash: hashes.join(","), type });
  return getJson<{ names: Record<string, string> }>(`/api/resolve?${params.toString()}`);
}

export function cancelRequest(id: string): void {
  try {
    void fetch(apiUrl(`/api/cancel?id=${encodeURIComponent(id)}`), {
      method: "POST",
      keepalive: true,
    }).catch(() => {});
  } catch {
    /* best effort */
  }
}

async function getJson<T>(url: string, signal?: AbortSignal): Promise<T> {
  // Tag the request so aborting it also cancels the server-side work: closing
  // the socket alone doesn't stop a non-streaming aggregation mid-flight.
  const reqId = newRequestId();
  let onAbort: (() => void) | undefined;
  if (signal) {
    if (signal.aborted) cancelRequest(reqId);
    else {
      onAbort = () => cancelRequest(reqId);
      signal.addEventListener("abort", onAbort, { once: true });
    }
  }
  try {
    const res = await fetch(apiUrl(url), {
      signal,
      headers: { "X-Request-Id": reqId },
    });
    if (!res.ok) {
      const text = await res.text().catch(() => "");
      throw new Error(text || `${res.status} ${res.statusText}`);
    }
    return (await res.json()) as T;
  } finally {
    if (signal && onAbort) signal.removeEventListener("abort", onAbort);
  }
}

export function fetchInfo(signal?: AbortSignal): Promise<InfoResponse> {
  return getJson<InfoResponse>("/api/info", signal);
}

export function fetchProcTree(signal?: AbortSignal): Promise<{ nodes: ProcTreeNode[] }> {
  const params = new URLSearchParams();
  return getJson<{ nodes: ProcTreeNode[] }>(
    `/api/viz/proctree?${withFile(params).toString()}`,
    signal,
  );
}

export function fetchViz(q: VizQuery, signal?: AbortSignal): Promise<VizResponse> {
  const params = new URLSearchParams({
    begin: String(Math.floor(q.begin)),
    end: String(Math.ceil(q.end)),
    summary: String(q.summary),
  });
  if (q.query && q.query.trim()) params.set("query", q.query.trim());
  if (q.limit && q.limit > 0) params.set("limit", String(q.limit));
  return getJson<VizResponse>(`/api/viz/events?${withFile(params).toString()}`, signal);
}

export const UNTIMED_PAGE = 200;

export function fetchUntimed(
  query: string,
  offset: number,
  signal?: AbortSignal,
): Promise<UntimedResponse> {
  const params = new URLSearchParams({ limit: String(UNTIMED_PAGE), offset: String(offset) });
  if (query.trim()) params.set("query", query.trim());
  return getJson<UntimedResponse>(`/api/viz/untimed?${withFile(params).toString()}`, signal);
}

export function fetchVizDensity(q: VizQuery, signal?: AbortSignal): Promise<VizDensityResponse> {
  const params = new URLSearchParams({
    begin: String(Math.floor(q.begin)),
    end: String(Math.ceil(q.end)),
    summary: String(q.summary),
  });
  if (q.query && q.query.trim()) params.set("query", q.query.trim());
  if (q.limit && q.limit > 0) params.set("limit", String(q.limit));
  if (q.lookback && q.lookback > 0) params.set("lookback", String(Math.ceil(q.lookback)));
  if (q.width && q.width > 0) params.set("width", String(Math.round(q.width)));
  if (q.groupBy && q.groupBy.trim()) params.set("group_by", q.groupBy.trim());
  return getJson<VizDensityResponse>(`/api/viz/density?${withFile(params).toString()}`, signal);
}

export interface VizBreaks {
  gaps: { begin: number; end: number }[];
  multi_run: boolean;
}

export function fetchVizBreaks(signal?: AbortSignal): Promise<VizBreaks> {
  const params = new URLSearchParams();
  return getJson<VizBreaks>(`/api/viz/breaks?${withFile(params).toString()}`, signal);
}

export function fetchVizCounters(
  begin: number,
  end: number,
  query: string,
  buckets: number,
  signal?: AbortSignal,
): Promise<VizCounters> {
  const params = new URLSearchParams({
    begin: String(Math.floor(begin)),
    end: String(Math.ceil(end)),
    buckets: String(buckets),
  });
  if (query && query.trim()) params.set("query", query.trim());
  return getJson<VizCounters>(`/api/viz/counters?${withFile(params).toString()}`, signal);
}

export function fetchCallTree(
  begin: number,
  end: number,
  query: string,
  byProcess: boolean,
  signal?: AbortSignal,
): Promise<CallTreeResponse> {
  const params = new URLSearchParams({
    begin: String(Math.floor(begin)),
    end: String(Math.ceil(end)),
  });
  if (query && query.trim()) params.set("query", query.trim());
  if (byProcess) params.set("group", "pid");
  return getJson<CallTreeResponse>(`/api/viz/calltree?${withFile(params).toString()}`, signal);
}

export function fetchHistogram(
  begin: number,
  end: number,
  query: string,
  signal?: AbortSignal,
): Promise<HistogramResponse> {
  const params = new URLSearchParams({
    begin: String(Math.floor(begin)),
    end: String(Math.ceil(end)),
  });
  if (query && query.trim()) params.set("query", query.trim());
  return getJson<HistogramResponse>(`/api/viz/histogram?${withFile(params).toString()}`, signal);
}

export function fetchVizStats(
  t0: number,
  t1: number,
  query: string,
  group?: string,
  signal?: AbortSignal,
): Promise<SelectionStats> {
  const params = new URLSearchParams({
    begin: String(Math.floor(t0)),
    end: String(Math.ceil(t1)),
  });
  if (query && query.trim()) params.set("query", query.trim());
  if (group) params.set("group", group);
  return getJson<SelectionStats>(`/api/viz/stats?${withFile(params).toString()}`, signal);
}

export function fetchColumns(signal?: AbortSignal): Promise<{ columns: string[]; ready: boolean }> {
  return getJson<{ columns: string[]; ready: boolean }>(
    `/api/viz/columns?${withFile(new URLSearchParams()).toString()}`,
    signal,
  );
}

export interface LayersResponse {
  layers: Record<string, string>; // operation name -> category
  total_files: number; // files declared by FH metadata
  io_files: number; // files actually read from or written to
}

export function fetchLayers(signal?: AbortSignal): Promise<LayersResponse> {
  return getJson<LayersResponse>(
    `/api/viz/layers?${withFile(new URLSearchParams()).toString()}`,
    signal,
  );
}

// ---- provenance (/api/prov/graph) ----------------------------------------
export interface ProvEntity {
  hash: string;
  type: string;
  id: string;
  store: string;
  uri: string;
  pids: number[];
}
export interface ProvActivity {
  aid: string;
  name: string;
  activity: string;
  pid: number;
  ts: number;
  dur: number;
  used: string[];
  generated: string[];
  invalidated?: string[];
  updated?: string[];
  cat?: string;
}
export interface ProvEntityRelation {
  relation: string; // contains | part_of | derived_from | revision_of | ...
  subject: string;
  object: string;
}
export interface ProvFile {
  fhash: string;
  path: string;
  mount: string;
  rel: string;
}
export interface ProvMount {
  path: string;
  fstype: string;
}
export interface ProvAccess {
  aid: string;
  fhash: string;
  path: string;
  op: "read" | "write" | "open" | "delete" | "meta";
  calls: number;
  bytes: number;
}
export interface ProvType {
  name: string;
  role: string; // input | output | intermediate
  description: string;
}
export interface ProvGraphResponse {
  entity_relations?: ProvEntityRelation[];
  types?: ProvType[];
  entities: ProvEntity[];
  activities: ProvActivity[];
  files?: ProvFile[];
  mounts?: ProvMount[];
  accesses?: ProvAccess[];
  entity_files?: { entity: string; fhash: string; path: string }[];
  stats: {
    entities: number;
    activities: number;
    entity_records: number;
    prov_events: number;
    cont_events: number;
    dangling_hashes: number;
    isolated_entities: number;
    truncated_activities: number;
    orphan_chunks: number;
    files?: number;
    mounts?: number;
    io_events?: number;
    io_attributed?: number;
    files_excluded?: number;
  };
}
export function fetchProvGraph(signal?: AbortSignal): Promise<ProvGraphResponse> {
  return getJson<ProvGraphResponse>("/api/prov/graph", signal);
}
