// Chrome Trace Event as returned (raw) by /api/v1/viz/events. Timestamps are
// normalized to the global minimum by the server, so ts is 0-based microseconds.
export interface TraceEvent {
  name: string;
  cat: string;
  pid: number | string;
  tid: number | string;
  ts: number;
  dur: number;
  ph: string;
  args?: Record<string, unknown>;
  depth?: number; // server-computed containment depth (live path only)
  [key: string]: unknown;
}

export interface VizMetadata {
  begin: number;
  end: number;
  count: number;
  limit: number;
  truncated: boolean;
  ts_normalized: boolean;
  global_min_timestamp_us: number;
  // Longest event duration in the scanned range (density endpoint only),
  // including events folded into density blocks. Drives the client's lookback.
  max_dur?: number;
}

export interface VizResponse {
  events: TraceEvent[];
  metadata: VizMetadata;
}

// One aggregated block of sub-pixel events (Perfetto-style density LOD).
export interface DensityBlock {
  name: string;
  pid: number;
  tid: number;
  ts: number;
  dur: number;
  count: number;
  total: number;
  depth?: number; // server-computed containment depth (live path only)
}

export interface VizDensityResponse {
  events: TraceEvent[];
  density: DensityBlock[];
  metadata: VizMetadata & { density_count?: number };
}

// Per-bucket I/O counters for bandwidth/IOPS tracks (GET /api/v1/viz/counters).
export interface VizCounters {
  begin: number;
  end: number;
  buckets: number;
  bucket_us: number;
  truncated: boolean;
  read_bytes: number[];
  write_bytes: number[];
  ops: number[];
}

export interface InfoResponse {
  file_count: number;
  // Omitted by the server when the index has no valid time bounds.
  time_range?: {
    min_timestamp_us: number;
    max_timestamp_us: number;
  };
  files?: unknown[];
}

export interface VizQuery {
  begin: number;
  end: number;
  summary: number;
  query?: string;
  limit?: number;
  lookback?: number; // scan back this far to catch events that overlap the window
  width?: number; // canvas width in px; sets the server's 1px fold cutoff
}

// One process in the inferred fork hierarchy (GET /api/v1/viz/proctree).
export interface ProcTreeNode {
  pid: number;
  parent: number; // -1 for a root
  spawn_ts: number; // normalized us of the parent's clone, 0 for a root
  first_ts: number;
  host?: string; // resolved hostname (node grouping), "" if unknown
  bytes?: number; // total I/O bytes for the process
  io_ops?: number; // count of I/O (POSIX/STDIO/IO) operations
  io_busy?: number; // I/O busy time in us
  rank?: string; // MPI/process rank from "PR" metadata, if present
}

// One node of the merged call tree (GET /api/v1/viz/calltree). `total` is
// inclusive us, `self` is total minus nested children, `count` is folded events.
export interface FlameNode {
  name: string;
  total: number;
  self: number;
  count: number;
  children: FlameNode[];
}

export interface CallTreeResponse {
  truncated: boolean;
  tree: FlameNode;
}

// GET /api/v1/viz/histogram: duration distribution of the matched events.
export interface HistBucket {
  lo: number;
  hi: number;
  count: number;
}

export interface HistogramResponse {
  count: number;
  min?: number;
  max?: number;
  mean?: number;
  p50?: number;
  p90?: number;
  p95?: number;
  p99?: number;
  truncated: boolean;
  buckets: HistBucket[];
}

export interface NameStat {
  name: string;
  count: number;
  total: number;
  avg: number;
  min: number;
  max: number;
}

// Response of GET /api/v1/viz/stats (aggregated server-side).
export interface SelectionStats {
  count: number;
  total_dur: number;
  wall: number;
  truncated: boolean;
  names: NameStat[];
}
