// Chrome Trace Event as returned (raw) by /api/viz/events. Timestamps are
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
  // DFTracer SELECTIVE-aggregation record (ph=3): dur is the aggregation window
  // and args carries dft_cnt/dur_sum. Individual events are gone from the trace.
  agg?: boolean;
  // A synthetic event extrapolated from an aggregate when zoomed below the
  // window: positioned uniformly, so the timing is an estimate, not real.
  est?: boolean;
  // Timing that is impossible against the trace's process lifetimes (a duration
  // longer than any process, or a span outside all of them), so the timestamps
  // are untrustworthy, e.g. a mixed-clock GPU event. Set by Timeline.setData.
  malformed_event?: boolean;
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

// One record written without a clock (ts 0, or a path schema's record without
// a time); ph 1 is an event, 3 an aggregated record. A path schema's pid is
// its entity value.
export interface UntimedRow {
  name: string;
  cat: string | null;
  pid: number | string;
  tid: number;
  dur: number | null;
  ph: number;
}

// Records the timeline cannot place: `count` covers every match, `events` is
// the page at `offset`, longest first.
export interface UntimedResponse {
  events: UntimedRow[];
  count: number;
  offset: number;
  limit: number;
}

// One aggregated block of sub-pixel events (Perfetto-style density LOD).
export interface DensityBlock {
  group?: string; // group_by value; absent when grouping is off or value missing
  name: string;
  pid: number;
  tid: number;
  ts: number;
  dur: number;
  count: number;
  total: number;
  depth?: number; // server-computed containment depth (live path only)
  // ph="C" counter blocks: aggregated numeric args.* over the bucket. `value` is
  // the mean reading; distinct counters (name.arg) each get their own block.
  counter?: boolean;
  value?: number;
}

export interface VizDensityResponse {
  events: TraceEvent[];
  density: DensityBlock[];
  // group_names maps raw group values (e.g. fhash) to display names.
  metadata: VizMetadata & {
    density_count?: number;
    group_names?: Record<string, string>;
  };
}

// Per-bucket I/O counters for bandwidth/IOPS tracks (GET /api/viz/counters).
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

// The served files' record schema: dftracer's decoder, or a path schema whose
// fields name its time, duration, entity and lane roles and its label ("" when
// it has none).
export interface TraceSchema {
  id: string;
  decoder: "dftracer" | "path";
  fields: {
    time: string;
    duration: string;
    entity: string;
    lane: string;
    label: string;
    entity_name: string;
  };
}

export interface InfoResponse {
  file_count: number;
  schema?: TraceSchema;
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
  duql?: string;
  limit?: number;
  lookback?: number; // scan back this far to catch events that overlap the window
  width?: number; // canvas width in px; sets the server's 1px fold cutoff
  groupBy?: string; // density grouping column (server group_by)
}

// One process in the inferred fork hierarchy (GET /api/viz/proctree).
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
  label?: string; // a path schema's text entity value (pid hashes it)
}

// One node of the merged call tree (GET /api/viz/calltree). `total` is
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

// GET /api/viz/histogram: duration distribution of the matched events.
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
  // Wall-clock time the operation was active (union of its event intervals), so
  // concurrent work is not summed. 0 when unavailable (whole-trace summary path).
  coverage?: number;
}

// Response of GET /api/viz/stats (aggregated server-side).
export interface SelectionStats {
  count: number;
  total_dur: number;
  wall: number;
  truncated: boolean;
  names: NameStat[];
}
