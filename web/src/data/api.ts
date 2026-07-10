import type {
  CallTreeResponse,
  HistogramResponse,
  InfoResponse,
  ProcTreeNode,
  SelectionStats,
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

async function getJson<T>(url: string, signal?: AbortSignal): Promise<T> {
  const res = await fetch(apiUrl(url), { signal });
  if (!res.ok) {
    const text = await res.text().catch(() => "");
    throw new Error(text || `${res.status} ${res.statusText}`);
  }
  return (await res.json()) as T;
}

export function fetchInfo(signal?: AbortSignal): Promise<InfoResponse> {
  return getJson<InfoResponse>("/api/v1/info", signal);
}

export function fetchProcTree(signal?: AbortSignal): Promise<{ nodes: ProcTreeNode[] }> {
  const params = new URLSearchParams();
  return getJson<{ nodes: ProcTreeNode[] }>(
    `/api/v1/viz/proctree?${withFile(params).toString()}`,
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
  return getJson<VizResponse>(`/api/v1/viz/events?${withFile(params).toString()}`, signal);
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
  return getJson<VizDensityResponse>(`/api/v1/viz/density?${withFile(params).toString()}`, signal);
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
  return getJson<VizCounters>(`/api/v1/viz/counters?${withFile(params).toString()}`, signal);
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
  return getJson<CallTreeResponse>(`/api/v1/viz/calltree?${withFile(params).toString()}`, signal);
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
  return getJson<HistogramResponse>(`/api/v1/viz/histogram?${withFile(params).toString()}`, signal);
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
  return getJson<SelectionStats>(`/api/v1/viz/stats?${withFile(params).toString()}`, signal);
}

export interface LayersResponse {
  layers: Record<string, string>; // operation name -> category
  total_files: number; // files declared by FH metadata
  io_files: number; // files actually read from or written to
}

export function fetchLayers(signal?: AbortSignal): Promise<LayersResponse> {
  return getJson<LayersResponse>(
    `/api/v1/viz/layers?${withFile(new URLSearchParams()).toString()}`,
    signal,
  );
}
