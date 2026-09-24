import type { DensityBlock, ProcTreeNode, TraceEvent } from "../data/types";
import { planAggregate } from "./aggregate";
import { colorFor, colorSlot, contrastText, DENSITY_GREY, sliceKey } from "./color";
import {
  formatBytesCompact,
  formatBytesPerSec,
  formatCompact,
  formatRate,
  formatTick,
  formatTime,
  niceStep,
} from "./format";
import { buildTimeAxis, type TimeAxis } from "./timeaxis";
import { vizTheme, type ThemeMode, type VizTheme } from "./theme";

export interface Viewport {
  begin: number;
  end: number;
}

interface Slice {
  ev: TraceEvent;
  ts: number;
  dur: number;
  laneIdx: number;
  depth: number;
  sdepth: number; // server-computed containment depth, or -1 (fall back to stacking)
  count: number; // >1 for aggregated density blocks and ph=3 aggregates
  total: number; // summed busy time (== dur for individual events)
  density: boolean;
  aggregated: boolean; // ph=3 record: dur is the window, count is dft_cnt
  est: boolean; // synthetic event extrapolated from an aggregate (estimated ts)
  malformed: boolean; // impossible timing vs process lifetimes (untrustworthy)
  fill: string; // resolved once here; the frame loop redraws every slice
}

interface Lane {
  key: string;
  pid: string;
  tid: string;
  rows: number;
  y: number; // top offset within content (below ruler), in css px
  kind: "host" | "proc" | "thread" | "counter";
  label: string;
  indent: number; // gutter indent, px
  collapsible: boolean;
  collapseKey: string; // key toggled in `collapsed`; "" for leaf threads
  flatten: boolean; // a summary row (host / collapsed proc): render load, not bars
  host: string;
  processFirst: boolean; // first lane of its process group
  soleThread: boolean; // process has exactly one thread
  depth: number; // fork-hierarchy depth (0 = root)
  // Present only on kind "counter": the ph="C" series drawn in this lane and the
  // source scope. `scope` is "node" (pid 0 emitters) or a specific pid; `overlay`
  // stacks several series normalized in one lane vs one series auto-scaled.
  counter?: { scope: "node" | number; series: string[]; overlay: boolean };
}

export interface Gap {
  laneIdx: number;
  label: string;
  t0: number;
  t1: number;
  dur: number;
}

// Ordered lane-grouping hierarchy. The default reproduces the classic
// host > process > thread gutter; any other order/subset uses the generic
// header/leaf layout. "col" groups by an event column (see setLaneGrouping's
// `column`), e.g. cat, name, or args.ret.
export type LaneGroupLevel = "host" | "pid" | "tid" | "col";
export const DEFAULT_LANE_GROUPS: readonly LaneGroupLevel[] = ["host", "pid", "tid"];

// Group for events missing the column (or when the column does not exist).
export const NONE_GROUP = "(none)";
// Overflow bucket when a column has more distinct values than fits in lanes.
export const OTHER_GROUP = "(other)";
const MAX_COL_GROUPS = 120;

// Joins the per-column values of a composite (multi-key) grouping. Must match
// the server's GROUP_SEP so density blocks and big events land in the same lane.
export const GROUP_SEP = "\x1f";

// Raw value of a single column ("" when missing/null/non-scalar). Dotted paths
// walk nested objects; bare names fall back into args.
const RESOLVED_PREFIX = "resolved.";

function rawGroupValue(rec: Record<string, unknown>, col: string): string {
  // A resolved column groups on its key field; the server resolves the key.
  if (col.startsWith(RESOLVED_PREFIX)) col = col.slice(RESOLVED_PREFIX.length).split(".")[0];
  let v: unknown;
  if (col.includes(".")) {
    v = col
      .split(".")
      .reduce<unknown>(
        (cur, key) =>
          cur !== null && typeof cur === "object"
            ? (cur as Record<string, unknown>)[key]
            : undefined,
        rec,
      );
  } else {
    v = rec[col];
    if (v == null) {
      const args = rec["args"];
      if (args !== null && typeof args === "object") v = (args as Record<string, unknown>)[col];
    }
  }
  if (v == null || typeof v === "object") return "";
  return String(v);
}

// Value of `col` in an event for lane grouping. A comma-separated `col`
// ("cat,fhash") yields the per-column raw values joined by GROUP_SEP (matching
// the server, so each component can resolve independently); a single column
// maps missing/empty to NONE_GROUP so no event is dropped.
export function eventGroupValue(ev: TraceEvent, col: string): string {
  const rec = ev as unknown as Record<string, unknown>;
  if (col.includes(",")) {
    return col
      .split(",")
      .map((c) => rawGroupValue(rec, c.trim()))
      .join(GROUP_SEP);
  }
  const s = rawGroupValue(rec, col);
  return s === "" ? NONE_GROUP : s;
}

const HASH_ALIAS: Record<string, string> = {
  fhash: "resolved.fhash.path",
  hhash: "resolved.hhash.name",
  exec_hash: "resolved.exec_hash.value",
  cmd_hash: "resolved.cmd_hash.value",
  cwd: "resolved.cwd.path",
};

// Add a column and, for hash columns, its resolved.* alias.
export function addGroupColumn(into: Set<string>, name: string): void {
  into.add(name);
  const bare = name.startsWith("args.") ? name.slice(5) : name;
  if (HASH_ALIAS[bare]) into.add(HASH_ALIAS[bare]);
}

// Harvest groupable column names from raw events: scalar top-level fields and
// args keys (bare names work via the args fallback), plus resolved.* aliases
// for hash columns. Samples a bounded prefix; accumulates into `into`. Used as
// a fallback until the authoritative /viz/columns list arrives.
export function collectGroupColumns(events: TraceEvent[], into: Set<string>): void {
  const SKIP_TOP = new Set(["pid", "tid", "ts", "dur", "ph", "id", "args", "depth"]);
  let n = 0;
  for (const ev of events) {
    if (n++ >= 300) break;
    const rec = ev as unknown as Record<string, unknown>;
    for (const [k, v] of Object.entries(rec)) {
      if (SKIP_TOP.has(k)) continue;
      if (v !== null && v !== "" && typeof v !== "object") addGroupColumn(into, k);
    }
    const args = rec["args"];
    if (args !== null && typeof args === "object") {
      for (const [k, v] of Object.entries(args as Record<string, unknown>)) {
        if (v !== null && v !== "" && typeof v !== "object") addGroupColumn(into, k);
      }
    }
  }
}

// Order two group values: numeric when both look numeric (so mhost 2 < 10),
// otherwise a natural compare that orders embedded numbers by value.
export function compareGroupKeys(a: string, b: string): number {
  const na = Number(a);
  const nb = Number(b);
  const aNum = a.trim() !== "" && Number.isFinite(na);
  const bNum = b.trim() !== "" && Number.isFinite(nb);
  if (aNum && bNum) return na - nb || (a < b ? -1 : a > b ? 1 : 0);
  if (aNum) return -1; // numbers before text
  if (bNum) return 1;
  return a.localeCompare(b, undefined, { numeric: true, sensitivity: "base" });
}

// Lowest free row for a span starting at `ts`, greedy interval partitioning:
// reuses any row whose occupant has ended, so staggered overlapping spans pack
// into max-concurrency rows (a pure stack would give every span a new row).
// Marks the chosen row busy until `end`.
export function takeRow(rowEnds: number[], ts: number, end: number): number {
  for (let r = 0; r < rowEnds.length; r++) {
    if (rowEnds[r] <= ts) {
      rowEnds[r] = end;
      return r;
    }
  }
  rowEnds.push(end);
  return rowEnds.length - 1;
}

// Map a raw group value (e.g. an fhash) to its server-resolved display name;
// unresolved values pass through unchanged.
export function resolveGroup(value: string, names?: Record<string, string>): string {
  if (value.includes(GROUP_SEP)) {
    // Composite key: resolve each component (empty -> (none), hash -> name) and
    // join for display.
    return value
      .split(GROUP_SEP)
      .map((part) => {
        if (part === "") return NONE_GROUP;
        const n = names?.[part];
        return n === undefined || n === "" ? part : n;
      })
      .join(" / ");
  }
  if (!names) return value;
  const n = names[value];
  return n === undefined || n === "" ? value : n;
}

export interface TimelineCallbacks {
  // Fired (debounced) when the target time window changes; the app turns this
  // into a /viz/events query.
  onRangeChange?: (begin: number, end: number) => void;
  onHover?: (ev: TraceEvent | null, clientX: number, clientY: number) => void;
  // Cursor over a gutter column header (track/i/o util/ops/i/o ops/bytes).
  onHeaderHover?: (key: string | null, clientX: number, clientY: number) => void;
  // Lane label under the cursor in the gutter, surfaced when rows are too short
  // to draw the label inline (null when not over a hidden-label lane).
  onLaneHover?: (label: string | null, clientX: number, clientY: number) => void;
  onSelect?: (ev: TraceEvent | null) => void;
  // Fired when the user finishes dragging a time-range selection (for stats).
  onSelectRange?: (t0: number, t1: number) => void;
  // Fired when the user finishes a rectangle selection: a time range, the lanes
  // it covers, and the operation names under the covered rows, so stats can
  // scope to exactly those tracks and operations.
  onSelectRect?: (
    t0: number,
    t1: number,
    lanes: { pid: string; tid: string }[],
    names: string[],
  ) => void;
  onSelectRangeClear?: () => void;
  // Fired when the set of ph="C" counter series in the current data changes, so
  // the app can populate the counter picker.
  onCounterSeries?: (series: CounterSeriesInfo[]) => void;
  // Hover over a counter lane: nearest reading of each series/source to the
  // cursor time (null when not over one).
  onCounterHover?: (info: CounterHoverInfo | null, clientX: number, clientY: number) => void;
  // Click on a counter lane: select it (like picking an event).
  onCounterSelect?: (info: CounterSelectInfo | null) => void;
}

// One counter reading at (or nearest) a queried time.
export interface CounterReading {
  name: string;
  pid: number;
  tid: number;
  value: number;
  ts: number;
}
export interface CounterHoverInfo {
  ts: number;
  series: CounterReading[];
}
export interface CounterSelectInfo {
  label: string;
  scope: "node" | number;
  ts: number;
  series: CounterReading[];
}

// Aggregate of one counter source over a time range.
export interface CounterRangeStat {
  name: string;
  pid: number;
  tid: number;
  scope: "node" | "proc";
  n: number;
  min: number;
  max: number;
  mean: number;
  first: number;
  last: number;
}

// A ph="C" counter series (e.g. "PAPI.PAPI_BR_INS", "cpu.user_pct"). Node-level
// counters emit from pid 0; process-level ones have one source per process.
export interface CounterSeriesInfo {
  name: string;
  sources: number; // distinct (pid,tid) emitters
  nodeLevel: boolean;
}

// One emitter of a counter series: a time-ordered set of bucket readings.
interface CounterSource {
  pid: number;
  tid: number;
  ts: number[]; // bucket centers (us), ascending
  val: number[];
}

const GUTTER_DEFAULT = 348;
const GUTTER_MIN = 180;
const GUTTER_MAX = 900;
const GUTTER_KEY = "dftracer.gutterW";
// Offsets from the gutter's right edge, so widening it all goes to the label.
const COL_UTIL_OFF = 220; // left edge of the I/O UTIL bar column
// Gutter width when the I/O columns are hidden (their width returned to the plot).
const GUTTER_NO_METRICS = Math.max(GUTTER_MIN, GUTTER_DEFAULT - (COL_UTIL_OFF - 16));
const COL_UTIL_W = 44;
const COL_OPS_OFF = 66; // right edge of the OPS (ops/s) column
const COL_BYTES_OFF = 12; // right edge of the BYTES column
const GUTTER_GRAB = 5; // px either side of the divider that starts a resize
const RULER_H = 28;
const ROW_H = 18;
const MIN_ROW_H = 2; // vertical-zoom floor: bars stay a visible hairline (no upper cap)
const LABEL_MIN_ROW_H = 12; // below this the metric columns/twist are hidden
const MIN_LABEL_FONT = 4; // smallest gutter-label font; scales up with row height
const LANE_GAP = 6;
const HOST_GAP = 12; // vertical separation between host groups
const TWIST_W = 14; // twisty hit-area / indent step per tree level
const COUNTER_ROWS = 2; // height (in event-rows) of a small-multiples counter lane
const COUNTER_ROWS_OVERLAY = 3; // taller lane when several series share it
// An event longer than the longest process by this factor is impossible, so its
// timing is malformed (the factor absorbs float slack, not real overruns).
const MALFORMED_DUR_FACTOR = 1.25;
const MIN_SPAN = 1; // microseconds
const EASE = 0.22;
const RANGE_DEBOUNCE_MS = 130;
// Zoom aggressiveness: exp(delta * SENSITIVITY). Wheel deltas are coalesced per
// animation frame and each frame applies at most ZOOM_MAX_STEP px of delta, so a
// momentum/high-resolution burst glides in smoothly instead of leaping.
const ZOOM_SENSITIVITY = 0.0025;
const ZOOM_MAX_STEP = 90;
const MINI_COLS = 600; // activity buckets across the whole trace
const MINI_SHADES = 24; // quantized activity shades, so colours are reused
const NAV_KEYS = new Set(["w", "a", "s", "d", "arrowleft", "arrowright", "arrowup", "arrowdown"]);

function clamp(v: number, lo: number, hi: number): number {
  return v < lo ? lo : v > hi ? hi : v;
}

function num(v: unknown, def = 0): number {
  const n = typeof v === "number" ? v : Number(v);
  return Number.isFinite(n) ? n : def;
}

export class Timeline {
  private canvas: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D;
  private cb: TimelineCallbacks;

  private dpr = 1;
  private cssW = 0;
  private cssH = 0;

  private totalSpan = 1;
  private live: Viewport = { begin: 0, end: 1 };
  private target: Viewport = { begin: 0, end: 1 };

  private slices: Slice[] = [];
  private slicesByKey = new Map<string, Slice[]>();
  private lanes: Lane[] = [];
  private contentH = 0;
  private scrollY = 0;
  private processLabels = new Map<string, string>();

  // Every (pid/tid) lane ever seen, so a track stays visible (empty) when the
  // current window has no events for it, instead of vanishing on zoom.
  private laneRegistry = new Set<string>();

  private groupSpec: LaneGroupLevel[] = [...DEFAULT_LANE_GROUPS];
  private groupColumn = ""; // comma-separated columns backing the "col" levels
  private colGroupSizes: number[] = []; // columns per "col" level (1=nested, >1=merged)
  private groupNames: Record<string, string> = {}; // hash -> resolved name
  private colorBy = ""; // "" colors by event name; else by this column's value
  private rowH = ROW_H; // lane row height in px; scaled by the vertical zoom
  private showMetrics = false; // I/O UTIL / OPS / BYTES gutter columns
  private gutterWithMetrics = GUTTER_DEFAULT; // gutter width to restore when re-showing metrics
  private knownColumns = new Set<string>(); // groupable columns seen in events

  // Fork hierarchy (by pid): DFS order, depth, parent, and spawn timestamp.
  private procOrder = new Map<number, number>();
  private procDepth = new Map<number, number>();
  private procParent = new Map<number, number>();
  private procSpawn = new Map<number, number>(); // parent's clone timestamp
  private procFirst = new Map<number, number>(); // child's first-event timestamp
  private hasRanks = false; // PR metadata present: order by rank, not host

  // Minimap (overview strip) state.
  private mini?: HTMLCanvasElement;
  private miniCtx?: CanvasRenderingContext2D;
  private miniW = 0;
  private miniH = 0;
  private miniActivity: number[] = [];
  private miniActivityMax = 1;
  // Whole-trace per-lane activity for the minimap heatmap (keyed pid/tid).
  private overviewLanes = new Map<string, Float64Array>();
  // The lane heatmap only changes with its data, size or theme, but the minimap
  // is redrawn every frame for the viewport box: keep the heatmap on its own
  // canvas and blit it instead of re-filling every cell.
  private miniLaneCanvas: HTMLCanvasElement | null = null;
  private miniLaneKey = "";
  private overviewVersion = 0;
  private overviewMax = 1;
  private ovOpsByPid = new Map<number, number>(); // total events per pid
  private ioBusyByPid = new Map<number, number>(); // I/O busy us per pid
  private miniDragging = false;
  private miniAnchorT = 0;
  private miniAnchorX = 0;
  private miniMoved = false;
  private miniSel: { t0: number; t1: number } | null = null;

  private searchTerm = "";
  private searchMatches: Slice[] = [];
  private searchMatchSet = new Set<Slice>();
  private searchIdx = -1;

  // Counter track (bandwidth) state.
  private counterCanvas?: HTMLCanvasElement;
  private counterCtx?: CanvasRenderingContext2D;
  private counterW = 0;
  private counterH = 0;
  private counters?: { begin: number; bucketUs: number; read: number[]; write: number[] };
  private counterPeak = 1; // bytes/sec

  // Longest process lifetime in the current data; the ceiling a real event's
  // duration cannot exceed (see setData's malformed-timing check).
  private procMaxLife = 0;
  private hideMalformed = false;
  // Real-time window covered by the process lifetimes (app-span markers): the
  // range of trustworthy activity, used to reframe when malformed events hide.
  private validStart = 0;
  private validEnd = 0;
  private hasValidWindow = false;
  // Subtracted from displayed times so the axis reads 0-based from the valid
  // window when malformed events (which dragged the origin back ~14d) are hidden.
  private timeOrigin = 0;

  // ph="C" counter track (PAPI / sys): series name -> per-process/node sources.
  // Rendered as nested lanes (see counterLanesFor / renderCounterLanes).
  private counterSeries = new Map<string, CounterSource[]>();
  private counterSelected: string[] = [];
  private counterMode: "multiples" | "overlay" = "overlay";

  private gaps: Gap[] = [];
  private showGaps = false;
  private hoveredGap: Gap | null = null;
  private selectedGap: Gap | null = null;

  // Timelapse axis: between-run idle spans (real us) compressed to a fixed
  // display width so active periods sit together. Viewport math runs in the
  // compressed (display) space; events carry real ts mapped through the knots.
  private runGaps: { begin: number; end: number }[] = [];
  private timelapse = false;
  private realTotal = 1;
  private axis: TimeAxis = buildTimeAxis([], 1, false);

  private selected: TraceEvent | null = null;
  private hovered: Slice | null = null;

  private dragging = false;
  private lastX = 0;
  private lastY = 0;
  private moved = false;
  private gutterDown: { x: number; y: number } | null = null;
  private gutter = GUTTER_NO_METRICS;
  private gutterResizing = false;
  private mouseX = GUTTER_NO_METRICS;
  private cursorInside = false;
  private keys = new Set<string>();
  private pendingZoom = 0;
  private zoomFocusX = this.gutter;
  private selecting = false;
  private selAnchorT = 0;
  private selAnchorY = 0;
  // y0/y1 (content coords, below the ruler) are present only for a rectangle
  // selection; a plain range covers the full height.
  private selection: { t0: number; t1: number; y0?: number; y1?: number } | null = null;

  private th: VizTheme = vizTheme("dark");
  private hostByPid = new Map<number, string>();
  private hostPids = new Map<string, number[]>();
  private bytesByPid = new Map<number, number>();
  private collapsed = new Set<string>();
  private raf = 0;
  private frameScheduled = false;
  private _dirty = true;
  private rangeTimer = 0;
  private ro?: ResizeObserver;

  constructor(canvas: HTMLCanvasElement, cb: TimelineCallbacks) {
    this.canvas = canvas;
    this.cb = cb;
    const ctx = canvas.getContext("2d");
    if (!ctx) throw new Error("2D canvas context unavailable");
    this.ctx = ctx;
    try {
      // Start in the metrics-off state, so cap a restored width to that layout;
      // showing the I/O columns restores the wider gutter via gutterWithMetrics.
      const saved = Number(localStorage.getItem(GUTTER_KEY));
      if (Number.isFinite(saved) && saved > 0)
        this.gutter = clamp(saved, GUTTER_MIN, GUTTER_NO_METRICS);
    } catch {
      /* storage unavailable; use the default width */
    }

    canvas.addEventListener("wheel", this.onWheel, { passive: false });
    canvas.addEventListener("mousedown", this.onMouseDown);
    window.addEventListener("mousemove", this.onMouseMove);
    window.addEventListener("mouseup", this.onMouseUp);
    canvas.addEventListener("mouseleave", this.onMouseLeave);
    canvas.addEventListener("dblclick", this.onDblClick);
    window.addEventListener("keydown", this.onKeyDown);
    window.addEventListener("keyup", this.onKeyUp);

    this.ro = new ResizeObserver(() => {
      this.resize();
    });
    this.ro.observe(canvas.parentElement ?? canvas);

    this.resize();
    this.requestFrame();
  }

  destroy(): void {
    cancelAnimationFrame(this.raf);
    if (this.rangeTimer) clearTimeout(this.rangeTimer);
    this.ro?.disconnect();
    this.canvas.removeEventListener("wheel", this.onWheel);
    this.canvas.removeEventListener("mousedown", this.onMouseDown);
    window.removeEventListener("mousemove", this.onMouseMove);
    window.removeEventListener("mouseup", this.onMouseUp);
    this.canvas.removeEventListener("mouseleave", this.onMouseLeave);
    this.canvas.removeEventListener("dblclick", this.onDblClick);
    window.removeEventListener("keydown", this.onKeyDown);
    window.removeEventListener("keyup", this.onKeyUp);
    this.mini?.removeEventListener("mousedown", this.onMiniDown);
    window.removeEventListener("mousemove", this.onMiniMove);
    window.removeEventListener("mouseup", this.onMiniUp);
  }

  setTheme(mode: ThemeMode): void {
    this.th = vizTheme(mode);
    this.overviewVersion++;
    this.invalidate();
  }

  setTotalSpan(span: number): void {
    this.realTotal = Math.max(span, MIN_SPAN);
    this.rebuildAxis();
    this.totalSpan = Math.max(this.toDisplay(this.realTotal), MIN_SPAN);
    this.target = { begin: 0, end: this.totalSpan };
    this.live = { begin: 0, end: this.totalSpan };
    this.invalidate();
    this.emitRange(true);
  }

  // Between-run idle spans (real us) plus whether to auto-enable compression.
  setBreaks(gaps: { begin: number; end: number }[], enable: boolean): void {
    this.runGaps = [...gaps].sort((a, b) => a.begin - b.begin);
    this.timelapse = enable && this.runGaps.length > 0;
    this.setTotalSpan(this.realTotal);
  }

  hasBreaks(): boolean {
    return this.runGaps.length > 0;
  }

  isTimelapse(): boolean {
    return this.timelapse;
  }

  setTimelapse(on: boolean): void {
    this.timelapse = on && this.runGaps.length > 0;
    this.setTotalSpan(this.realTotal);
    this.resetView();
  }

  private rebuildAxis(): void {
    this.axis = buildTimeAxis(this.runGaps, this.realTotal, this.timelapse);
  }

  private toDisplay(r: number): number {
    return this.axis.toDisplay(r);
  }

  private toReal(dv: number): number {
    return this.axis.toReal(dv);
  }

  // Display us -> screen x (viewport-space, no gap remap).
  private xdOf(d: number): number {
    const span = this.live.end - this.live.begin;
    return this.gutter + ((d - this.live.begin) / span) * this.plotW();
  }

  resetView(): void {
    this.target = { begin: this.viewLo(), end: this.viewHi() };
    this.scrollY = 0;
    this.invalidate();
    this.emitRange(false);
  }

  getViewport(): Viewport {
    return { begin: this.toReal(this.target.begin), end: this.toReal(this.target.end) };
  }

  setSelected(ev: TraceEvent | null): void {
    this.selected = ev;
    this.invalidate();
  }

  // Human-readable label per process id (e.g. resolved host name).
  setProcessLabels(labels: Map<string, string>): void {
    this.processLabels = labels;
    this.layoutLanes();
  }

  // Fork hierarchy: order processes by DFS (children after parent) and record
  // depth + spawn time so lanes indent and spawn arrows can be drawn.
  setProcTree(nodes: ProcTreeNode[]): void {
    this.overviewVersion++;
    this.procOrder = new Map();
    this.procDepth = new Map();
    this.procParent = new Map();
    this.procSpawn = new Map();
    this.procFirst = new Map();
    const children = new Map<number, number[]>();
    const pids = new Set<number>();
    for (const n of nodes) {
      pids.add(n.pid);
      this.procParent.set(n.pid, n.parent);
      this.procSpawn.set(n.pid, n.spawn_ts);
      this.procFirst.set(n.pid, n.first_ts);
      if (n.parent >= 0) {
        const c = children.get(n.parent);
        if (c) c.push(n.pid);
        else children.set(n.parent, [n.pid]);
      }
    }
    const firstTs = new Map(nodes.map((n) => [n.pid, n.first_ts]));
    const byFirst = (a: number, b: number) => (firstTs.get(a) ?? 0) - (firstTs.get(b) ?? 0);
    // Rank-order the roots so procOrder reflects rank; layoutLanes then groups
    // lanes by node and orders nodes by their lowest rank.
    const rankOf = new Map<number, number>();
    for (const n of nodes) {
      const r = n.rank != null && n.rank !== "" ? Number(n.rank) : NaN;
      if (Number.isFinite(r)) rankOf.set(n.pid, r);
    }
    this.hasRanks = rankOf.size > 0;
    const byRank = (a: number, b: number) =>
      (rankOf.get(a) ?? Infinity) - (rankOf.get(b) ?? Infinity) || byFirst(a, b);
    const roots = nodes
      .filter((n) => n.parent < 0 || !pids.has(n.parent))
      .map((n) => n.pid)
      .sort(this.hasRanks ? byRank : byFirst);
    let ord = 0;
    const visit = (pid: number, depth: number) => {
      this.procOrder.set(pid, ord++);
      this.procDepth.set(pid, depth);
      for (const c of (children.get(pid) ?? []).slice().sort(byFirst)) {
        visit(c, depth + 1);
      }
    };
    for (const r of roots) visit(r, 0);
    // The tree usually arrives after the first data render, so re-order the
    // lanes already on screen instead of waiting for the next fetch.
    this.relayoutLanes();
  }

  // Re-sort existing lanes into fork-hierarchy DFS order and re-flow their y
  // offsets, remapping slices to their new lane index. Used when the process
  // tree arrives after lanes were first built in plain pid order.
  private relayoutLanes(): void {
    this.layoutLanes();
  }

  // --- Gap / idle analysis --------------------------------------------------

  setShowGaps(v: boolean): void {
    this.showGaps = v;
    if (!v) {
      this.hoveredGap = null;
      this.selectedGap = null;
    }
    this.invalidate();
  }

  // Idle periods per lane: the complement of its busy coverage. A density block
  // tiles the full bucket width but is only busy for `total`, so it counts as
  // busy only when total/dur clears IDLE_FRAC; real slices are always busy.
  private computeGaps(): void {
    const IDLE_FRAC = 0.02;
    const byLane = new Map<number, Slice[]>();
    for (const s of this.slices) {
      let a = byLane.get(s.laneIdx);
      if (!a) {
        a = [];
        byLane.set(s.laneIdx, a);
      }
      a.push(s);
    }
    const gaps: Gap[] = [];
    for (const [laneIdx, arr] of byLane) {
      const lane = this.lanes[laneIdx];
      const label = this.processLabels.get(lane.pid) ?? lane.key;
      const busy: Array<[number, number]> = [];
      for (const s of arr) {
        // The synthetic app span covers the whole process lifetime; it is a
        // container, not activity, so it must not mask a lane's idle periods.
        if (s.ev.cat === "dftracer") continue;
        const full = Math.max(s.dur, 0);
        if (s.density && (full <= 0 || Math.min(s.total, full) / full < IDLE_FRAC)) continue;
        busy.push([s.ts, s.ts + full]);
      }
      busy.sort((a, b) => a[0] - b[0]);
      // Tolerance to skip float-rounding slivers between contiguous buckets.
      const eps = this.totalSpan * 1e-6;
      let curEnd = -Infinity;
      let started = false;
      for (const [b0, b1] of busy) {
        if (!started) {
          curEnd = b1;
          started = true;
          continue;
        }
        if (b0 > curEnd + eps) {
          gaps.push({ laneIdx, label, t0: curEnd, t1: b0, dur: b0 - curEnd });
          curEnd = b1;
        } else if (b1 > curEnd) {
          curEnd = b1;
        }
      }
    }
    gaps.sort((a, b) => b.dur - a.dur);
    this.gaps = gaps;
  }

  topGaps(n: number): Gap[] {
    // Resolve the label now (process labels may arrive after computeGaps ran).
    return this.gaps.slice(0, n).map((g) => {
      const lane = this.lanes[g.laneIdx];
      return { ...g, label: this.processLabels.get(lane.pid) ?? lane.key };
    });
  }

  // Scrollable time bounds. With malformed events hidden, panning/zooming is
  // confined to the trustworthy window so the view cannot wander into the empty
  // range the hidden events opened up; otherwise it is the whole trace.
  private viewLo(): number {
    return this.hideMalformed && this.hasValidWindow ? this.validStart : 0;
  }
  private viewHi(): number {
    return this.hideMalformed && this.hasValidWindow ? this.validEnd : this.totalSpan;
  }

  // Fit the viewport to [t0, t1] with a little padding.
  focusRange(t0: number, t1: number): void {
    const pad = Math.max((t1 - t0) * 0.3, 1);
    const begin = clamp(t0 - pad, this.viewLo(), this.viewHi());
    const end = clamp(t1 + pad, this.viewLo(), this.viewHi());
    if (end > begin) {
      this.target = { begin, end };
      this.invalidate();
      this.emitRange(false);
    }
  }

  private renderGaps(ctx: CanvasRenderingContext2D): void {
    if (!this.showGaps) return;
    ctx.save();
    ctx.beginPath();
    ctx.rect(this.gutter, RULER_H, this.cssW - this.gutter, this.cssH - RULER_H);
    ctx.clip();
    const active = this.selectedGap ?? this.hoveredGap;
    for (const g of this.gaps) {
      const x0 = this.xOf(g.t0);
      const x1 = this.xOf(g.t1);
      if (x1 - x0 < 3) continue; // skip sub-few-pixel gaps
      if (x1 < this.gutter || x0 > this.cssW) continue;
      const lane = this.lanes[g.laneIdx];
      const y = RULER_H - this.scrollY + lane.y;
      const hh = lane.rows * this.rowH;
      if (y + hh < RULER_H || y > this.cssH) continue;
      const x = Math.max(x0, this.gutter);
      const w = Math.min(x1, this.cssW) - x;
      const yy = Math.max(y, RULER_H);
      const on = g === active;
      ctx.fillStyle = on ? this.th.gapActiveFill : this.th.gapFill;
      ctx.fillRect(x, yy, w, hh - (y < RULER_H ? RULER_H - y : 0));
      if (on) {
        ctx.strokeStyle = this.th.gapStroke;
        ctx.lineWidth = 1;
        ctx.strokeRect(x + 0.5, yy + 0.5, w - 1, hh - 1 - (y < RULER_H ? RULER_H - y : 0));
      }
    }
    ctx.restore();
    if (active) this.renderGapReadout(ctx, active);
  }

  private renderGapReadout(ctx: CanvasRenderingContext2D, g: Gap): void {
    const x0 = this.xOf(g.t0);
    const x1 = this.xOf(g.t1);
    const lane = this.lanes[g.laneIdx];
    const y = RULER_H - this.scrollY + lane.y;
    const label = `idle ${formatTime(g.dur)}`;
    ctx.save();
    ctx.font = "11px ui-monospace, SFMono-Regular, Menlo, monospace";
    const tw = ctx.measureText(label).width;
    const padX = 6;
    const bw = tw + padX * 2;
    const bh = 18;
    const bx = clamp((x0 + x1) / 2 - bw / 2, this.gutter + 2, this.cssW - bw - 2);
    const by = clamp(y - bh - 4, RULER_H + 2, this.cssH - bh - 2);
    ctx.fillStyle = this.th.ttBg;
    ctx.strokeStyle = this.th.gapStroke;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.rect(bx, by, bw, bh);
    ctx.fill();
    ctx.stroke();
    ctx.fillStyle = this.th.gapStroke;
    ctx.textBaseline = "middle";
    ctx.fillText(label, bx + padX, by + bh / 2 + 0.5);
    ctx.restore();
  }

  // --- Search: highlight slices whose name matches; dim the rest -------------

  setSearch(term: string): number {
    this.searchTerm = term.trim().toLowerCase();
    this.searchIdx = -1;
    this.recomputeMatches();
    this.invalidate();
    return this.searchMatches.length;
  }

  private recomputeMatches(): void {
    this.searchMatches = [];
    this.searchMatchSet = new Set();
    if (!this.searchTerm) return;
    for (const s of this.slices) {
      if (sliceKey(s.ev).toLowerCase().includes(this.searchTerm)) {
        this.searchMatches.push(s);
        this.searchMatchSet.add(s);
      }
    }
    this.searchMatches.sort((a, b) => a.ts - b.ts);
  }

  // Move to the next/prev match, centering the viewport and selecting it.
  // Returns the 1-based match position (0 if none).
  focusMatch(dir: 1 | -1): number {
    const n = this.searchMatches.length;
    if (n === 0) return 0;
    this.searchIdx = (this.searchIdx + dir + n) % n;
    const s = this.searchMatches[this.searchIdx];
    const center = this.toDisplay(s.ts + s.dur / 2);
    const span = this.target.end - this.target.begin;
    const begin = clamp(
      center - span / 2,
      this.viewLo(),
      Math.max(this.viewLo(), this.viewHi() - span),
    );
    this.target = { begin, end: begin + span };
    this.selected = s.ev;
    this.cb.onSelect?.(s.ev);
    this.invalidate();
    this.emitRange(false);
    return this.searchIdx + 1;
  }

  // The longest process lifetime seen in the last setData (0 if unknown).
  maxProcLifetime(): number {
    return this.procMaxLife;
  }

  setHideMalformed(v: boolean): void {
    if (this.hideMalformed === v) return;
    this.hideMalformed = v;
    this.timeOrigin = v && this.hasValidWindow ? this.validStart : 0;
    // Re-lay out so malformed events drop out of (or back into) the lane row
    // counts, not just the render.
    this.layoutLanes();
    // Hidden events can sit far outside the real activity, so the trustworthy
    // window is a small slice of the axis; jump straight to it. Snap live and
    // target together: easing there refetches on every frame.
    if (v && this.hasValidWindow) {
      // Start exactly at validStart so the axis reads from 0 (no negative pad);
      // a little room on the right keeps the last events off the edge.
      const begin = this.validStart;
      const end = clamp(
        this.validEnd + (this.validEnd - this.validStart) * 0.02,
        0,
        this.totalSpan,
      );
      if (end > begin) {
        this.target = { begin, end };
        this.live = { begin, end };
        this.emitRange(false);
      }
    }
    this.invalidate();
  }

  // Ground-truth timing bounds from the per-process app-span markers the server
  // appends (cat "dftracer", args.num_events): the earliest any process began
  // and the longest any ran. An event that starts before the former or lasts
  // longer than the latter is physically impossible, so its timing is malformed.
  private timingBounds(
    events: TraceEvent[],
  ): { validStart: number; validEnd: number; maxLife: number } | null {
    let validStart = Infinity;
    let validEnd = -Infinity;
    let maxLife = 0;
    for (const ev of events) {
      const a = ev.args as Record<string, unknown> | undefined;
      if (ev.cat !== "dftracer" || !a || a.num_events === undefined) continue;
      const ts = num(ev.ts);
      const dur = num(ev.dur);
      if (!Number.isFinite(ts) || !Number.isFinite(dur)) continue;
      if (ts < validStart) validStart = ts;
      if (ts + dur > validEnd) validEnd = ts + dur;
      if (dur > maxLife) maxLife = dur;
    }
    return maxLife > 0 && Number.isFinite(validStart) ? { validStart, validEnd, maxLife } : null;
  }

  setData(
    events: TraceEvent[],
    density: DensityBlock[] = [],
    groupNames?: Record<string, string>,
  ): void {
    const bounds = this.timingBounds(events);
    this.procMaxLife = bounds?.maxLife ?? 0;
    this.validStart = bounds?.validStart ?? 0;
    this.validEnd = bounds?.validEnd ?? 0;
    this.hasValidWindow = bounds != null && this.validEnd > this.validStart;
    this.timeOrigin = this.hideMalformed && this.hasValidWindow ? this.validStart : 0;
    const byLane = new Map<string, Slice[]>();
    const push = (key: string, s: Slice) => {
      let arr = byLane.get(key);
      if (!arr) {
        arr = [];
        byLane.set(key, arr);
      }
      arr.push(s);
    };
    collectGroupColumns(events, this.knownColumns);
    this.groupNames = groupNames ?? {};
    const colActive = this.colGroupingActive();
    for (const ev of events) {
      if (ev.ph === "M") continue; // metadata
      const ts = num(ev.ts);
      const dur = num(ev.dur);
      if (!Number.isFinite(ts) || dur < 0) continue;
      // ph=3 aggregate: dur is the window, args.dft_cnt/dur_sum the real stats.
      // Marking the event aggregated lights up the grey fill and inspector notes.
      const agg = ev.agg === true;
      const est = ev.est === true;
      const a = (ev.args ?? {}) as Record<string, unknown>;
      const cnt = agg ? num(a.dft_cnt) : 1;
      const busy = agg ? num(a.dur_sum) : dur;
      if (agg) (ev as Record<string, unknown>).aggregated = true;
      // A duration longer than any process, or a start before the earliest one,
      // is impossible; the slack around validStart is one process lifetime.
      const malformed = bounds
        ? (!agg && dur > bounds.maxLife * MALFORMED_DUR_FACTOR) ||
          ts < bounds.validStart - bounds.maxLife
        : false;
      ev.malformed_event = malformed;
      const key = colActive
        ? `${ev.pid}/${ev.tid}\u0000${eventGroupValue(ev, this.groupColumn)}`
        : `${ev.pid}/${ev.tid}`;
      // Extrapolated aggregate events keep the server's depth: it peeks at the
      // containment level (right under the active call stack) without occupying
      // a row, so they never add a band of their own.
      const sdepth = typeof ev.depth === "number" ? ev.depth : -1;
      push(key, {
        ev,
        ts,
        dur,
        laneIdx: 0,
        depth: 0,
        sdepth,
        count: agg && cnt > 0 ? cnt : 1,
        total: agg && Number.isFinite(busy) ? busy : dur,
        density: false,
        aggregated: agg,
        est,
        malformed,
        fill: this.fillFor(ev, agg),
      });
    }
    // Aggregated density blocks render as slices too. The server echoes the
    // group_by value per block; blocks without one land in "(none)".
    const counterBlocks: DensityBlock[] = [];
    for (const b of density) {
      // ph="C" counter blocks carry a numeric reading, not a call slice: divert
      // them to the counter track instead of drawing them as fake bars.
      if (b.counter) {
        counterBlocks.push(b);
        continue;
      }
      const synthetic = {
        name: b.name,
        cat: "",
        pid: b.pid,
        tid: b.tid,
        ts: b.ts,
        dur: b.dur,
        ph: "X",
        args: {},
        count: b.count,
        total: b.total,
        aggregated: true,
      } as unknown as TraceEvent;
      const bkey = colActive
        ? `${b.pid}/${b.tid}\u0000${b.group ? b.group : NONE_GROUP}`
        : `${b.pid}/${b.tid}`;
      push(bkey, {
        ev: synthetic,
        ts: b.ts,
        dur: b.dur,
        laneIdx: 0,
        depth: 0,
        sdepth: typeof b.depth === "number" ? b.depth : -1,
        count: b.count,
        total: b.total,
        density: true,
        aggregated: false,
        est: false,
        malformed: false,
        fill: this.fillFor(synthetic, true),
      });
    }

    // Persist every lane so tracks don't vanish when they have no events in the
    // current window (Perfetto keeps them). New lanes are added, never removed.
    for (const k of byLane.keys()) this.laneRegistry.add(k);
    this.slicesByKey = byLane;
    this.ingestCounters(counterBlocks);
    this.layoutLanes();
  }

  // Fold ph="C" density blocks into per-series, per-source point sets. Each
  // block is one bucket reading for a (series, pid, tid); node-level counters
  // emit from pid 0, process-level ones once per process.
  private ingestCounters(blocks: DensityBlock[]): void {
    const acc = new Map<string, Map<string, CounterSource>>();
    for (const b of blocks) {
      if (typeof b.value !== "number" || !Number.isFinite(b.value)) continue;
      const name = b.name || b.group || "";
      if (!name) continue;
      let sm = acc.get(name);
      if (!sm) {
        sm = new Map();
        acc.set(name, sm);
      }
      const skey = `${b.pid}/${b.tid}`;
      let src = sm.get(skey);
      if (!src) {
        src = { pid: b.pid, tid: b.tid, ts: [], val: [] };
        sm.set(skey, src);
      }
      src.ts.push(b.ts + b.dur / 2);
      src.val.push(b.value);
    }
    const series = new Map<string, CounterSource[]>();
    const info: CounterSeriesInfo[] = [];
    for (const [name, sm] of acc) {
      const srcs: CounterSource[] = [];
      for (const s of sm.values()) {
        // Blocks may arrive out of order; sort each source by time once.
        const order = s.ts.map((_, i) => i).sort((a, c) => s.ts[a] - s.ts[c]);
        srcs.push({
          pid: s.pid,
          tid: s.tid,
          ts: order.map((i) => s.ts[i]),
          val: order.map((i) => s.val[i]),
        });
      }
      srcs.sort((a, c) => a.pid - c.pid || a.tid - c.tid);
      series.set(name, srcs);
      info.push({ name, sources: srcs.length, nodeLevel: srcs.every((s) => s.pid === 0) });
    }
    info.sort((a, c) => a.name.localeCompare(c.name));
    this.counterSeries = series;
    this.counterSelected = this.counterSelected.filter((n) => series.has(n));
    this.cb.onCounterSeries?.(info);
    this.invalidate();
  }

  // Merge, never replace: the per-window HH scan is empty at full view and
  // would otherwise wipe the proctree-provided host map.
  setHosts(hosts: Map<number, string>): void {
    let changed = false;
    for (const [pid, host] of hosts) {
      if (this.hostByPid.get(pid) !== host) {
        this.hostByPid.set(pid, host);
        changed = true;
      }
    }
    if (changed) this.layoutLanes();
  }

  setBytes(bytes: Map<number, number>): void {
    this.bytesByPid = bytes;
    this.invalidate();
  }

  toggleCollapse(key: string): void {
    if (this.collapsed.has(key)) this.collapsed.delete(key);
    else this.collapsed.add(key);
    this.layoutLanes();
  }

  // spec has one "col" level per column GROUP; colGroupSizes gives each group's
  // column count (1 = a nested single-column level, >1 = merged columns sharing
  // a lane). column is the flattened column list in composite order.
  setLaneGrouping(spec: LaneGroupLevel[], column = "", colGroupSizes: number[] = []): void {
    // Dedup builtin levels; keep every "col".
    const clean = spec.filter((v, i) => v === "col" || spec.indexOf(v) === i);
    if (clean.length === 0) return;
    const col = clean.includes("col") ? column : "";
    const sizesKey = colGroupSizes.join(",");
    if (
      clean.join() === this.groupSpec.join() &&
      col === this.groupColumn &&
      sizesKey === this.colGroupSizes.join()
    )
      return;
    // Lane keys embed the raw composite, so a change to the column set
    // invalidates the bucketed data; the app refetches after that.
    const shapeChanged = col !== this.groupColumn;
    this.groupSpec = clean;
    this.groupColumn = col;
    this.colGroupSizes = colGroupSizes;
    this.collapsed.clear(); // collapse keys are hierarchy-specific
    if (shapeChanged) {
      this.laneRegistry.clear();
      this.slicesByKey = new Map();
    }
    this.layoutLanes();
  }

  laneGrouping(): LaneGroupLevel[] {
    return [...this.groupSpec];
  }

  // Vertical (lane-density) zoom: scale the row height so more or fewer lanes
  // fit on screen. Independent of the time (horizontal) zoom.
  rowHeight(): number {
    return this.rowH;
  }
  setRowHeight(px: number): void {
    const h = Math.max(MIN_ROW_H, Math.round(px)); // floor only; no upper cap
    if (h === this.rowH) return;
    this.rowH = h;
    this.layoutLanes(); // lane.y depends on rowH
    this.clampScroll();
    this.requestFrame();
  }
  zoomRows(delta: number): void {
    this.setRowHeight(this.rowH + delta);
  }
  // Show or hide the I/O UTIL / OPS / BYTES gutter columns; the lane label takes
  // the freed width.
  metricsShown(): boolean {
    return this.showMetrics;
  }
  setShowMetrics(v: boolean): void {
    if (v === this.showMetrics) return;
    this.showMetrics = v;
    // Reclaim the columns' width for the plot: narrow the gutter when hiding
    // them, and restore its previous width when showing them again.
    if (!v) {
      this.gutterWithMetrics = this.gutter;
      this.gutter = clamp(this.gutter - (COL_UTIL_OFF - 16), GUTTER_MIN, GUTTER_MAX);
    } else {
      this.gutter = clamp(this.gutterWithMetrics, GUTTER_MIN, GUTTER_MAX);
    }
    this.clampScroll();
    this.invalidate();
  }
  // Shrink rows just enough that every lane fits in the viewport at once.
  fitRows(): void {
    const totalRows = this.lanes.reduce((n, l) => n + l.rows, 0);
    if (totalRows === 0) return;
    const gaps = this.lanes.length * LANE_GAP;
    const avail = this.cssH - RULER_H - gaps;
    this.setRowHeight(Math.floor(avail / totalRows));
  }

  knownGroupColumns(): string[] {
    return [...this.knownColumns].sort();
  }

  colorField(): string {
    return this.colorBy || "name";
  }

  // Switch the color dimension and recolor existing slices in place (no
  // refetch): folded density blocks turn neutral grey since they mix values.
  setColorBy(col: string): void {
    const c = col === "name" ? "" : col;
    if (c === this.colorBy) return;
    this.colorBy = c;
    for (const arr of this.slicesByKey.values())
      for (const s of arr) s.fill = this.fillFor(s.ev, s.density || s.aggregated);
    this.invalidate();
  }

  private fillFor(ev: TraceEvent, density: boolean): string {
    if (!this.colorBy) return colorFor(sliceKey(ev));
    if (density) return DENSITY_GREY;
    return colorFor(colorSlot(this.colorBy, eventGroupValue(ev, this.colorBy)));
  }

  addKnownColumns(names: string[]): void {
    for (const n of names) addGroupColumn(this.knownColumns, n);
  }

  private colGroupingActive(): boolean {
    return this.groupColumn !== "" && this.groupSpec.includes("col");
  }

  private isDefaultGrouping(): boolean {
    return this.groupSpec.join() === DEFAULT_LANE_GROUPS.join();
  }

  private layoutLanes(): void {
    if (this.isDefaultGrouping()) this.layoutLanesDefault();
    else this.layoutLanesGeneric();
  }

  // Counter lanes for a scope: "node" (pid-0 emitters, e.g. cpu/mem) or a
  // process pid (PAPI). Overlay mode packs the applicable selection into one
  // normalized lane; small-multiples emits one auto-scaled lane per series.
  private counterLanesFor(scope: "node" | number, indent: number, host: string): Lane[] {
    const applies = (name: string): boolean => {
      const srcs = this.counterSeries.get(name);
      if (!srcs) return false;
      return scope === "node" ? srcs.some((s) => s.pid === 0) : srcs.some((s) => s.pid === scope);
    };
    const sel = this.counterSelected.filter(applies);
    if (sel.length === 0) return [];
    const base = {
      pid: "",
      tid: "",
      y: 0,
      kind: "counter" as const,
      indent,
      collapsible: false,
      collapseKey: "",
      flatten: false,
      host,
      processFirst: false,
      soleThread: false,
      depth: 0,
    };
    if (this.counterMode === "overlay") {
      return [
        {
          ...base,
          key: `counter:${scope}`,
          rows: COUNTER_ROWS_OVERLAY,
          label: `${sel.length} counters`,
          counter: { scope, series: sel, overlay: true },
        },
      ];
    }
    return sel.map((name) => ({
      ...base,
      key: `counter:${scope}:${name}`,
      rows: COUNTER_ROWS,
      label: name,
      counter: { scope, series: [name], overlay: false },
    }));
  }

  // Build the gutter tree: host -> process (fork DFS) -> thread. Collapsed
  // groups fold their descendants' slices into one summary row so load still
  // shows. Assigns each slice a target lane + stack depth and flows y.
  private layoutLanesDefault(): void {
    const orderOf = (pid: number) => this.procOrder.get(pid) ?? 1e9 + pid;
    // Band by node/host. procOrder is rank-ordered, so bands sort by their
    // lowest rank (below) and lanes within a band sort by rank.
    const hostOf = (pid: number) => this.hostByPid.get(pid) ?? "unknown";
    const parentOf = (pid: number) => this.procParent.get(pid);

    const pids = new Set<number>();
    const tidsByPid = new Map<number, string[]>();
    for (const k of this.laneRegistry) {
      const slash = k.indexOf("/");
      const pid = Number(k.slice(0, slash));
      pids.add(pid);
      let a = tidsByPid.get(pid);
      if (!a) {
        a = [];
        tidsByPid.set(pid, a);
      }
      a.push(k.slice(slash + 1));
    }
    for (const a of tidsByPid.values()) a.sort((x, y) => Number(x) - Number(y));

    const inHost = (pid: number, h: string) => pids.has(pid) && hostOf(pid) === h;
    const childrenOf = new Map<number, number[]>();
    for (const pid of pids) {
      const par = parentOf(pid);
      if (par != null && par >= 0 && inHost(par, hostOf(pid))) {
        let a = childrenOf.get(par);
        if (!a) {
          a = [];
          childrenOf.set(par, a);
        }
        a.push(pid);
      }
    }
    for (const a of childrenOf.values()) a.sort((x, y) => orderOf(x) - orderOf(y));

    const hostPids = new Map<string, number[]>();
    for (const pid of pids) {
      const h = hostOf(pid);
      let a = hostPids.get(h);
      if (!a) {
        a = [];
        hostPids.set(h, a);
      }
      a.push(pid);
    }
    const hosts = [...hostPids.keys()].sort((a, b) => {
      const ma = Math.min(...hostPids.get(a)!.map(orderOf));
      const mb = Math.min(...hostPids.get(b)!.map(orderOf));
      return ma - mb || (a < b ? -1 : 1);
    });
    this.hostPids = hostPids;

    const lanes: Lane[] = [];
    const targetOf = new Map<string, number>(); // pid/tid -> lane index
    const collapsedH = (h: string) => this.collapsed.has("host:" + h);
    const collapsedP = (p: number) => this.collapsed.has("pid:" + p);
    const routeAll = (pid: number, idx: number) => {
      for (const t of tidsByPid.get(pid) ?? []) targetOf.set(pid + "/" + t, idx);
      for (const c of childrenOf.get(pid) ?? []) routeAll(c, idx);
    };

    for (const host of hosts) {
      const hIdx = lanes.length;
      const singleHost = hosts.length === 1 && host === "unknown";
      if (!singleHost) {
        lanes.push({
          key: "host:" + host,
          pid: "",
          tid: "",
          rows: 1,
          y: 0,
          kind: "host",
          label: host,
          indent: 0,
          collapsible: true,
          collapseKey: "host:" + host,
          flatten: true,
          host,
          processFirst: true,
          soleThread: false,
          depth: 0,
        });
        if (collapsedH(host)) {
          for (const pid of hostPids.get(host)!) routeAll(pid, hIdx);
          continue;
        }
      }
      const base = singleHost ? 0 : 1;
      // Node-level counters (cpu/mem) sit right under the host header.
      for (const cl of this.counterLanesFor("node", base * TWIST_W, host)) lanes.push(cl);
      const roots = hostPids
        .get(host)!
        .filter((pid) => {
          const par = parentOf(pid);
          return !(par != null && par >= 0 && inHost(par, host));
        })
        .sort((a, b) => orderOf(a) - orderOf(b));

      const walk = (pid: number, d: number): void => {
        const tids = tidsByPid.get(pid) ?? [];
        const sole = tids.length === 1;
        const kids = childrenOf.get(pid) ?? [];
        const collapsed = collapsedP(pid);
        const pIdx = lanes.length;
        lanes.push({
          key: "pid:" + pid,
          pid: String(pid),
          tid: sole ? tids[0] : "",
          rows: 1,
          y: 0,
          kind: "proc",
          label: this.processLabels.get(String(pid)) ?? "proc " + pid,
          indent: (base + d) * TWIST_W,
          collapsible: true,
          collapseKey: "pid:" + pid,
          flatten: collapsed,
          host,
          processFirst: true,
          soleThread: sole,
          depth: d,
        });
        if (collapsed) {
          routeAll(pid, pIdx);
          return;
        }
        // Per-process counters (PAPI) nest under the process, above its threads.
        for (const cl of this.counterLanesFor(pid, (base + d + 1) * TWIST_W, host)) lanes.push(cl);
        if (sole) {
          targetOf.set(pid + "/" + tids[0], pIdx);
        } else {
          for (const t of tids) {
            targetOf.set(pid + "/" + t, lanes.length);
            lanes.push({
              key: pid + "/" + t,
              pid: String(pid),
              tid: t,
              rows: 1,
              y: 0,
              kind: "thread",
              label: "thread " + t,
              indent: (base + d + 1) * TWIST_W,
              collapsible: false,
              collapseKey: "",
              flatten: false,
              host,
              processFirst: false,
              soleThread: false,
              depth: d,
            });
          }
        }
        for (const c of kids) walk(c, d + 1);
      };
      for (const r of roots) walk(r, 0);
    }

    this.finishLayout(lanes, targetOf, new Set());
  }

  // Generic layout for a non-default grouping: an ordered hierarchy over lane
  // attributes. Non-leaf levels render as collapsible summary headers; the last
  // level renders bars. Lanes that merge several (pid, tid) keys stack on the
  // client, since server depths are per-thread.
  private layoutLanesGeneric(): void {
    const spec = this.groupSpec;
    interface Leaf {
      key: string;
      pid: number;
      tid: string;
      host: string;
      group: string;
    }
    const leaves: Leaf[] = [];
    const hostPids = new Map<string, number[]>();
    for (const k of this.laneRegistry) {
      const nul = k.indexOf("\u0000");
      const base = nul === -1 ? k : k.slice(0, nul);
      const group = nul === -1 ? NONE_GROUP : k.slice(nul + 1);
      const slash = base.indexOf("/");
      const pid = Number(base.slice(0, slash));
      const host = this.hostByPid.get(pid) ?? "unknown";
      leaves.push({ key: k, pid, tid: base.slice(slash + 1), host, group });
      let a = hostPids.get(host);
      if (!a) {
        a = [];
        hostPids.set(host, a);
      }
      if (!a.includes(pid)) a.push(pid);
    }
    this.hostPids = hostPids;

    const orderOf = (pid: number) => this.procOrder.get(pid) ?? 1e9 + pid;
    // Each "col" level covers a contiguous slice of the composite value: a
    // one-column slice is a nested level; a multi-column slice is a merged group
    // sharing a lane ("a / b"). colGroupSizes gives each level's slice width.
    const colNames = this.groupColumn ? this.groupColumn.split(",") : [];
    const colIndexOf: number[] = [];
    let cc = 0;
    for (const lvl of spec) colIndexOf.push(lvl === "col" ? cc++ : -1);
    const colRanges: Array<[number, number]> = [];
    for (let acc = 0, i = 0; i < this.colGroupSizes.length; i++) {
      colRanges.push([acc, acc + this.colGroupSizes[i]]);
      acc += this.colGroupSizes[i];
    }
    const rangeOf = (colIdx: number): [number, number] => colRanges[colIdx] ?? [colIdx, colIdx + 1];
    const valOf = (l: Leaf, lvl: LaneGroupLevel, colIdx: number) => {
      if (lvl === "host") return l.host;
      if (lvl === "pid") return String(l.pid);
      if (lvl === "tid") return l.tid;
      const [s, e] = rangeOf(colIdx);
      return l.group.split(GROUP_SEP).slice(s, e).join(GROUP_SEP) || NONE_GROUP;
    };
    const labelOf = (lvl: LaneGroupLevel, v: string, colIdx: number) => {
      if (lvl === "host") return v;
      if (lvl === "pid") return this.processLabels.get(v) ?? "proc " + v;
      if (lvl === "tid") return "thread " + v;
      const resolved = resolveGroup(v, this.groupNames);
      // A single-column level names its dimension ("cat: POSIX"); a merged group
      // shows just the combined value.
      const [s, e] = rangeOf(colIdx);
      return e - s === 1 && colNames[s] ? `${colNames[s]}: ${resolved}` : resolved;
    };

    const lanes: Lane[] = [];
    const targetOf = new Map<string, number>();
    const merged = new Set<number>();
    const colActive = this.colGroupingActive();
    const routeAll = (members: Leaf[], idx: number) => {
      for (const m of members) targetOf.set(m.key, idx);
      // Merged multi-key lanes, and any column-split lane (a subset of one
      // thread's events), need client-side depth packing.
      if (members.length > 1 || colActive) merged.add(idx);
    };

    const build = (subset: Leaf[], levelIdx: number, path: string, indent: number): void => {
      const lvl = spec[levelIdx];
      const colIdx = colIndexOf[levelIdx];
      const groups = new Map<string, Leaf[]>();
      for (const l of subset) {
        const v = valOf(l, lvl, colIdx);
        let a = groups.get(v);
        if (!a) {
          a = [];
          groups.set(v, a);
        }
        a.push(l);
      }
      const keys = [...groups.keys()].sort((a, b) => {
        if (lvl === "pid") return orderOf(Number(a)) - orderOf(Number(b));
        if (lvl === "tid") return Number(a) - Number(b);
        if (lvl === "col") {
          // Missing/overflow buckets last; numeric values sort numerically,
          // everything else lexicographically.
          if (a === NONE_GROUP) return 1;
          if (b === NONE_GROUP) return -1;
          return compareGroupKeys(a, b);
        }
        const ma = Math.min(...groups.get(a)!.map((l) => orderOf(l.pid)));
        const mb = Math.min(...groups.get(b)!.map((l) => orderOf(l.pid)));
        return ma - mb || (a < b ? -1 : 1);
      });
      // A high-cardinality column would explode the lane count; overflow into
      // one "(other)" bucket instead.
      let entries: [string, Leaf[]][] = keys.map((v) => [v, groups.get(v)!]);
      if (lvl === "col" && entries.length > MAX_COL_GROUPS) {
        const keep = entries.slice(0, MAX_COL_GROUPS);
        const rest = entries.slice(MAX_COL_GROUPS).flatMap(([, m]) => m);
        keep.push([OTHER_GROUP, rest]);
        entries = keep;
      }
      const isLeaf = levelIdx === spec.length - 1;
      for (const [v, members] of entries) {
        const laneKey = path + lvl + ":" + v;
        const idx = lanes.length;
        if (isLeaf) {
          lanes.push({
            key: laneKey,
            pid: lvl === "pid" || members.length === 1 ? String(members[0].pid) : "",
            tid: lvl === "tid" || members.length === 1 ? members[0].tid : "",
            rows: 1,
            y: 0,
            kind: lvl === "tid" ? "thread" : "proc",
            label: labelOf(lvl, v, colIdx),
            indent,
            collapsible: false,
            collapseKey: "",
            flatten: false,
            host: members[0].host,
            processFirst: levelIdx === 0,
            soleThread: false,
            depth: levelIdx,
          });
          routeAll(members, idx);
        } else {
          lanes.push({
            key: laneKey,
            pid: "",
            tid: "",
            rows: 1,
            y: 0,
            // "host" kind drives band gaps and host-level column aggregation,
            // so only host levels get it; other headers render as proc rows.
            kind: lvl === "host" ? "host" : "proc",
            label: labelOf(lvl, v, colIdx),
            indent,
            collapsible: true,
            collapseKey: laneKey,
            flatten: true,
            host: lvl === "host" ? v : members[0].host,
            processFirst: true,
            soleThread: false,
            depth: levelIdx,
          });
          if (this.collapsed.has(laneKey)) routeAll(members, idx);
          else build(members, levelIdx + 1, laneKey + "|", indent + TWIST_W);
        }
      }
    };
    build(leaves, 0, "", 0);
    this.finishLayout(lanes, targetOf, merged);
  }

  // Route slices to their lanes, stack them, and flow lane y offsets.
  // `clientStack` lanes merge several (pid, tid) keys, so the server depth
  // does not apply and the client open-stack is used instead.
  private finishLayout(
    lanes: Lane[],
    targetOf: Map<string, number>,
    clientStack: Set<number>,
  ): void {
    // Anchor the viewport to the lane at its top before the row counts change
    // (they shift as zoom folds/unfolds events), so relayout never jumps the
    // user somewhere else vertically.
    const anchor = this.scrollAnchor();
    const byLane = new Map<number, Slice[]>();
    for (const [key, arr] of this.slicesByKey) {
      const idx = targetOf.get(key);
      if (idx == null) continue;
      let a = byLane.get(idx);
      if (!a) {
        a = [];
        byLane.set(idx, a);
      }
      // Hidden malformed events must not reserve rows either, or the lane keeps
      // a tall empty gap where their (14-day) bars would have stacked.
      for (const s of arr) {
        if (this.hideMalformed && s.malformed) continue;
        a.push(s);
      }
    }

    const slices: Slice[] = [];
    for (const [idx, arr] of byLane) {
      const lane = lanes[idx];
      const useClient = clientStack.has(idx);
      arr.sort((a, b) => a.ts - b.ts || b.dur - a.dur);
      const rowEnds: number[] = [];
      for (const s of arr) {
        s.laneIdx = idx;
        // Prefer the server-computed depth (stable across zoom); fall back to
        // greedy row packing for slices that do not supply one.
        if (lane.flatten) s.depth = 0;
        else if (!useClient && s.sdepth >= 0) s.depth = s.sdepth;
        else if (s.density) {
          // Density blocks peek at the next free row without occupying it.
          let r = 0;
          while (r < rowEnds.length && rowEnds[r] > s.ts) r++;
          s.depth = r;
        } else {
          s.depth = takeRow(rowEnds, s.ts, s.ts + Math.max(s.dur, 0));
        }
        lane.rows = Math.max(lane.rows, s.depth + 1);
        slices.push(s);
      }
      // A hidden malformed event that enclosed the real work left every
      // remaining slice one row deeper than it needs to be. Shift each lane's
      // stack up to fill row 0, preserving the relative call-stack nesting.
      if (this.hideMalformed && arr.length) {
        let minDepth = Infinity;
        for (const s of arr) if (s.depth < minDepth) minDepth = s.depth;
        if (minDepth > 0 && minDepth !== Infinity) {
          for (const s of arr) s.depth -= minDepth;
          lane.rows = Math.max(1, lane.rows - minDepth);
        }
      }
    }

    let y = 0;
    let prevKind = "";
    for (const lane of lanes) {
      if (lane.kind === "host" && prevKind) y += HOST_GAP;
      lane.y = y;
      y += lane.rows * this.rowH + LANE_GAP;
      prevKind = lane.kind;
    }

    this.slices = slices;
    this.lanes = lanes;
    this.contentH = y;
    this.restoreScrollAnchor(anchor);
    this.computeMiniActivity();
    this.recomputeMatches();
    this.computeGaps();
    this.clampScroll();
    this.invalidate();
  }

  // --- coordinate transforms -------------------------------------------------

  private colUtil(): number {
    return this.gutter - COL_UTIL_OFF;
  }

  private colOps(): number {
    return this.gutter - COL_OPS_OFF;
  }

  private colBytes(): number {
    return this.gutter - COL_BYTES_OFF;
  }

  // Widen or narrow the track panel; the label column absorbs the difference.
  setGutterWidth(w: number): void {
    const next = clamp(Math.round(w), GUTTER_MIN, GUTTER_MAX);
    if (next === this.gutter) return;
    this.gutter = next;
    try {
      localStorage.setItem(GUTTER_KEY, String(next));
    } catch {
      /* storage unavailable; the width just does not persist */
    }
    this.invalidate();
  }

  private plotW(): number {
    return Math.max(1, this.cssW - this.gutter);
  }

  private xOf(ts: number): number {
    return this.xdOf(this.toDisplay(ts));
  }

  private timeOf(x: number): number {
    const span = this.live.end - this.live.begin;
    return this.live.begin + ((x - this.gutter) / this.plotW()) * span;
  }

  // --- interaction -----------------------------------------------------------

  private localPos(e: MouseEvent): { x: number; y: number } {
    const r = this.canvas.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }

  // Perfetto-style wheel/trackpad:
  //   - Ctrl/Cmd+wheel or trackpad pinch (macOS reports pinch as ctrl+wheel):
  //     zoom at the cursor.
  //   - Two-finger horizontal swipe (deltaX) or Shift+wheel: pan time.
  //   - Two-finger vertical swipe (deltaY): scroll lanes.
  private onWheel = (e: WheelEvent): void => {
    e.preventDefault();
    const { x } = this.localPos(e);
    if (e.ctrlKey || e.metaKey) {
      // Normalize line/page wheel modes to pixels, then accumulate; the rAF loop
      // applies the coalesced delta so bursts don't leap.
      const dy =
        e.deltaMode === 1 ? e.deltaY * 16 : e.deltaMode === 2 ? e.deltaY * this.cssH : e.deltaY;
      this.pendingZoom += dy;
      this.zoomFocusX = x;
      this.requestFrame();
      return;
    }
    if (e.shiftKey) {
      // Shift+wheel: vertical (lane-density) zoom - scroll up grows rows, down
      // shrinks them so more lanes fit.
      const dy =
        e.deltaMode === 1 ? e.deltaY * 16 : e.deltaMode === 2 ? e.deltaY * this.cssH : e.deltaY;
      if (dy !== 0) this.zoomRows(dy < 0 ? 2 : -2);
      return;
    }
    const span = this.target.end - this.target.begin;
    // Trackpad horizontal swipe pans; a plain vertical wheel scrolls the lanes.
    if (e.deltaX !== 0) this.panBy((e.deltaX / this.plotW()) * span);
    if (e.deltaY !== 0) {
      this.scrollY += e.deltaY;
      this.clampScroll();
      this.invalidate();
    }
  };

  private zoomAt(x: number, deltaY: number): void {
    const anchor = this.timeOf(x);
    const span = this.target.end - this.target.begin;
    const factor = Math.exp(deltaY * ZOOM_SENSITIVITY);
    const newSpan = clamp(span * factor, MIN_SPAN, this.viewHi() - this.viewLo());
    const frac = clamp((anchor - this.target.begin) / span, 0, 1);
    let begin = anchor - frac * newSpan;
    let end = begin + newSpan;
    if (begin < 0) {
      begin = 0;
      end = newSpan;
    }
    if (end > this.totalSpan) {
      end = this.totalSpan;
      begin = end - newSpan;
    }
    this.target = { begin, end };
    this.invalidate();
    this.emitRange(false);
  }

  private panBy(dt: number): void {
    const span = this.target.end - this.target.begin;
    const begin = clamp(
      this.target.begin + dt,
      this.viewLo(),
      Math.max(this.viewLo(), this.viewHi() - span),
    );
    this.target = { begin, end: begin + span };
    this.invalidate();
    this.emitRange(false);
  }

  private onKeyDown = (e: KeyboardEvent): void => {
    const el = document.activeElement;
    if (el && (el.tagName === "INPUT" || el.tagName === "TEXTAREA")) return;
    const k = e.key.toLowerCase();
    // Vertical (lane-density) zoom: '-' shrinks rows (see more lanes), '='/'+'
    // grows them, '0' fits every lane on screen. Distinct from w/s time zoom.
    if (k === "-" || k === "_") {
      this.zoomRows(-3);
      e.preventDefault();
      return;
    }
    if (k === "=" || k === "+") {
      this.zoomRows(3);
      e.preventDefault();
      return;
    }
    if (k === "0") {
      this.fitRows();
      e.preventDefault();
      return;
    }
    if (NAV_KEYS.has(k)) {
      this.keys.add(k);
      e.preventDefault();
      this.requestFrame(); // start the animation loop while keys are held
    }
  };

  private onKeyUp = (e: KeyboardEvent): void => {
    this.keys.delete(e.key.toLowerCase());
  };

  // Continuous WASD/arrow navigation, applied once per animation frame so held
  // keys pan/zoom smoothly (Perfetto-style: A/D pan, W/S zoom at the cursor).
  private applyKeys(): void {
    if (this.keys.size === 0) return;
    const span = this.target.end - this.target.begin;
    if (this.keys.has("a") || this.keys.has("arrowleft")) this.panBy(-span * 0.02);
    if (this.keys.has("d") || this.keys.has("arrowright")) this.panBy(span * 0.02);
    if (this.keys.has("w") || this.keys.has("arrowup")) this.zoomAt(this.mouseX, -10);
    if (this.keys.has("s") || this.keys.has("arrowdown")) this.zoomAt(this.mouseX, 10);
  }

  private onMouseDown = (e: MouseEvent): void => {
    const { x, y } = this.localPos(e);
    if (Math.abs(x - this.gutter) <= GUTTER_GRAB) {
      this.gutterResizing = true;
      return;
    }
    // Drag on the ruler, or Shift+drag anywhere, selects a time range for stats.
    if (x >= this.gutter && (y < RULER_H || e.shiftKey)) {
      this.selecting = true;
      this.selAnchorT = this.timeOf(x);
      this.selection = { t0: this.selAnchorT, t1: this.selAnchorT };
      this.invalidate();
      return;
    }
    // Ctrl/Cmd + drag in the plot paints a rectangle (time x lanes) for stats.
    if ((e.ctrlKey || e.metaKey) && x >= this.gutter && y >= RULER_H) {
      this.selecting = true;
      this.selAnchorT = this.timeOf(x);
      // y bounds are stored in content space so the box stays on its lanes when
      // the user scrolls (screen y = contentY + RULER_H - scrollY).
      this.selAnchorY = y - RULER_H + this.scrollY;
      this.selection = {
        t0: this.selAnchorT,
        t1: this.selAnchorT,
        y0: this.selAnchorY,
        y1: this.selAnchorY,
      };
      this.invalidate();
      return;
    }
    if (x < this.gutter && y > RULER_H) {
      // Gutter: a potential collapse click; never a pan/scroll drag.
      this.gutterDown = { x, y };
      return;
    }
    this.dragging = true;
    this.moved = false;
    this.lastX = x;
    this.lastY = y;
  };

  private onMouseMove = (e: MouseEvent): void => {
    const { x, y } = this.localPos(e);
    this.mouseX = x;
    if (this.gutterResizing) {
      this.canvas.style.cursor = "col-resize";
      this.setGutterWidth(x);
      return;
    }
    if (this.selecting) {
      const t = clamp(this.timeOf(x), this.viewLo(), this.viewHi());
      const isRect = this.selection?.y0 !== undefined;
      const cy = clamp(y, RULER_H, this.cssH) - RULER_H + this.scrollY;
      this.selection = {
        t0: Math.min(this.selAnchorT, t),
        t1: Math.max(this.selAnchorT, t),
        ...(isRect
          ? {
              y0: Math.min(this.selAnchorY, cy),
              y1: Math.max(this.selAnchorY, cy),
            }
          : {}),
      };
      this.invalidate();
      return;
    }
    if (this.dragging) {
      const dx = x - this.lastX;
      const dy = y - this.lastY;
      if (Math.abs(dx) + Math.abs(dy) > 2) this.moved = true;
      const span = this.target.end - this.target.begin;
      const dt = -(dx / this.plotW()) * span;
      const begin = clamp(this.target.begin + dt, this.viewLo(), this.viewHi() - span);
      this.target = { begin, end: begin + span };
      this.scrollY -= dy;
      this.clampScroll();
      this.lastX = x;
      this.lastY = y;
      this.invalidate();
      this.emitRange(false);
      return;
    }
    // Off-canvas (window mousemove fires during drags): no hover. Leaves the
    // header-help alone so HTML metric labels can own it off-canvas.
    if (x < 0 || x > this.cssW || y < 0 || y > this.cssH) {
      if (this.hovered || this.hoveredGap) {
        this.hovered = null;
        this.hoveredGap = null;
        this.invalidate();
      }
      this.cursorInside = false;
      this.cb.onHover?.(null, 0, 0);
      this.cb.onCounterHover?.(null, 0, 0);
      this.cb.onLaneHover?.(null, 0, 0);
      return;
    }
    if (Math.abs(x - this.gutter) <= GUTTER_GRAB) {
      this.canvas.style.cursor = "col-resize";
      return;
    }
    this.cursorInside = x >= this.gutter && y >= RULER_H && x <= this.cssW;
    if (this.cursorInside) this.canvas.style.cursor = "crosshair";
    else if (x < this.gutter && y > RULER_H && this.gutterRowAt(y))
      this.canvas.style.cursor = "pointer";
    else this.canvas.style.cursor = "default";
    const hit = this.hitTest(x, y);
    const gap = hit ? null : this.gapAt(x, y);
    if (hit !== this.hovered || gap !== this.hoveredGap || this.cursorInside) {
      this.hovered = hit;
      this.hoveredGap = gap;
      this.invalidate();
    }
    this.cb.onHover?.(hit?.ev ?? null, e.clientX, e.clientY);
    const chit = hit ? null : this.counterAt(x, y);
    this.cb.onCounterHover?.(
      chit ? { ts: chit.ts - this.timeOrigin, series: chit.readings } : null,
      e.clientX,
      e.clientY,
    );
    this.cb.onHeaderHover?.(this.headerKeyAt(x, y), e.clientX, e.clientY);
    // When rows are too short to show labels inline, surface the label on hover.
    const laneLabel =
      x < this.gutter && y > RULER_H && this.rowH < LABEL_MIN_ROW_H
        ? (this.laneAtY(y)?.label ?? null)
        : null;
    this.cb.onLaneHover?.(laneLabel, e.clientX, e.clientY);
  };

  // Gutter column-header key under the cursor (for metric help tooltips).
  private headerKeyAt(x: number, y: number): string | null {
    if (y > RULER_H || x > this.gutter) return null;
    if (x < 120) return "track";
    if (x < 228) return "i/o util";
    if (x < 288) return "ops";
    return "bytes";
  }

  private onMouseUp = (e: MouseEvent): void => {
    if (this.gutterResizing) {
      this.gutterResizing = false;
      return;
    }
    if (this.gutterDown) {
      const g = this.gutterDown;
      this.gutterDown = null;
      const { x, y } = this.localPos(e);
      if (Math.abs(x - g.x) + Math.abs(y - g.y) < 4) {
        const row = this.gutterRowAt(y);
        if (row?.collapsible) this.toggleCollapse(row.collapseKey);
      }
      return;
    }
    if (this.selecting) {
      this.selecting = false;
      const sel = this.selection;
      const span = this.live.end - this.live.begin;
      if (sel && sel.t1 - sel.t0 > span * 0.002) {
        if (sel.y0 !== undefined && sel.y1 !== undefined) {
          const rt0 = this.toReal(sel.t0);
          const rt1 = this.toReal(sel.t1);
          this.cb.onSelectRect?.(
            rt0,
            rt1,
            this.lanesInYRange(sel.y0, sel.y1),
            this.namesInRect(rt0, rt1, sel.y0, sel.y1),
          );
        } else {
          this.cb.onSelectRange?.(this.toReal(sel.t0), this.toReal(sel.t1));
        }
      } else {
        this.selection = null;
        this.cb.onSelectRangeClear?.();
      }
      this.invalidate();
      return;
    }
    if (this.dragging && !this.moved) {
      const { x, y } = this.localPos(e);
      const hit = this.hitTest(x, y);
      // A click on empty plot clears both the picked event and any range.
      if (!hit && x > this.gutter && this.selection) {
        this.selection = null;
        this.cb.onSelectRangeClear?.();
      }
      this.selectedGap = hit ? null : this.gapAt(x, y);
      this.selected = hit?.ev ?? null;
      this.cb.onSelect?.(this.selected);
      const chit = hit ? null : this.counterAt(x, y);
      this.cb.onCounterSelect?.(
        chit
          ? {
              label: chit.lane.label,
              scope: chit.lane.counter!.scope,
              ts: chit.ts - this.timeOrigin,
              series: chit.readings,
            }
          : null,
      );
      this.invalidate();
    }
    this.dragging = false;
  };

  hasSelection(): boolean {
    return this.selection !== null;
  }

  clearSelection(): void {
    this.selection = null;
    this.invalidate();
  }

  // Lanes whose rows intersect the content y-range [y0, y1] (lane.y is content
  // space too). A thread lane maps to its (pid, tid); a summary row (host, or
  // collapsed process) stands for every tid of its pid (tid ""), and a host row
  // for all its pids.
  private lanesInYRange(y0: number, y1: number): { pid: string; tid: string }[] {
    const seen = new Set<string>();
    const out: { pid: string; tid: string }[] = [];
    const add = (pid: string, tid: string) => {
      const key = `${pid} ${tid}`;
      if (seen.has(key)) return;
      seen.add(key);
      out.push({ pid, tid });
    };
    for (const lane of this.lanes) {
      const ly0 = lane.y;
      const ly1 = ly0 + Math.max(1, lane.rows) * this.rowH;
      if (ly1 <= y0 || ly0 >= y1) continue;
      if (lane.kind === "counter") continue; // no pid/tid; not an event scope
      if (lane.kind === "host") {
        for (const pid of this.hostPids.get(lane.host) ?? []) add(String(pid), "");
      } else if (lane.flatten) {
        add(lane.pid, "");
      } else {
        add(lane.pid, lane.tid);
      }
    }
    return out;
  }

  // Distinct operation names whose slice falls inside the rectangle: its row
  // (lane.y + depth) overlaps the content y-range and its time overlaps
  // [t0, t1] (real time, as stored on the slice). Lets stats scope to the exact
  // rows the user dragged over, not the whole track.
  private namesInRect(t0: number, t1: number, y0: number, y1: number): string[] {
    const names = new Set<string>();
    for (const s of this.slices) {
      const lane = this.lanes[s.laneIdx];
      const cy0 = lane.y + s.depth * this.rowH;
      if (cy0 + this.rowH <= y0 || cy0 >= y1) continue;
      if (s.ts + s.dur <= t0 || s.ts >= t1) continue;
      const n = String(s.ev.name ?? "");
      if (n) names.add(n);
    }
    return [...names];
  }

  private onMouseLeave = (): void => {
    this.cursorInside = false;
    if (this.hovered) this.hovered = null;
    this.invalidate();
    this.cb.onHover?.(null, 0, 0);
    this.cb.onHeaderHover?.(null, 0, 0);
  };

  private onDblClick = (e: WheelEvent | MouseEvent): void => {
    const { x } = this.localPos(e as MouseEvent);
    const anchor = this.timeOf(x);
    const span = this.target.end - this.target.begin;
    const newSpan = clamp(span * 0.4, MIN_SPAN, this.viewHi() - this.viewLo());
    const begin = clamp(anchor - newSpan / 2, this.viewLo(), this.viewHi() - newSpan);
    this.target = { begin, end: begin + newSpan };
    this.invalidate();
    this.emitRange(false);
  };

  private hitTest(x: number, y: number): Slice | null {
    if (x < this.gutter || y < RULER_H) return null;
    let best: Slice | null = null;
    for (const s of this.slices) {
      const sx = this.xOf(s.ts);
      const sw = Math.max(this.xOf(s.ts + s.dur) - sx, 1);
      if (x < sx || x > sx + sw) continue;
      const lane = this.lanes[s.laneIdx];
      const sy = RULER_H - this.scrollY + lane.y + s.depth * this.rowH;
      if (y < sy || y > sy + this.rowH) continue;
      if (!best || s.depth >= best.depth) best = s;
    }
    return best;
  }

  // Topmost visible gap under the cursor (only when gaps are shown).
  private gapAt(x: number, y: number): Gap | null {
    if (!this.showGaps || x < this.gutter || y < RULER_H) return null;
    let best: Gap | null = null;
    for (const g of this.gaps) {
      const x0 = this.xOf(g.t0);
      const x1 = this.xOf(g.t1);
      if (x1 - x0 < 3 || x < x0 || x > x1) continue;
      const lane = this.lanes[g.laneIdx];
      const gy = RULER_H - this.scrollY + lane.y;
      const gh = lane.rows * this.rowH;
      if (y < gy || y > gy + gh) continue;
      // Smallest gap wins: nested lanes overlap, favor the most specific.
      if (!best || g.dur < best.dur) best = g;
    }
    return best;
  }

  private clampScroll(): void {
    const maxScroll = Math.max(0, this.contentH - (this.cssH - RULER_H));
    this.scrollY = clamp(this.scrollY, 0, maxScroll);
  }

  // The lane at the top of the viewport and the scroll offset into it, so the
  // same content can be kept in place across a relayout.
  private scrollAnchor(): { key: string; off: number } | null {
    for (const lane of this.lanes) {
      const bottom = lane.y + lane.rows * this.rowH + LANE_GAP;
      if (this.scrollY < bottom) return { key: lane.key, off: this.scrollY - lane.y };
    }
    return null;
  }

  private restoreScrollAnchor(anchor: { key: string; off: number } | null): void {
    if (!anchor) return;
    const lane = this.lanes.find((l) => l.key === anchor.key);
    if (lane) this.scrollY = lane.y + anchor.off;
  }

  private emitRange(immediate: boolean): void {
    if (this.rangeTimer) clearTimeout(this.rangeTimer);
    const fire = () =>
      this.cb.onRangeChange?.(this.toReal(this.target.begin), this.toReal(this.target.end));
    if (immediate) {
      fire();
    } else {
      this.rangeTimer = window.setTimeout(fire, RANGE_DEBOUNCE_MS);
    }
  }

  // Pixels actually drawn across; the server folds anything narrower than one.
  viewportWidth(): number {
    return Math.max(1, this.canvas.width || Math.round(this.cssW * this.dpr));
  }

  // --- render loop -----------------------------------------------------------

  resize(): void {
    const parent = this.canvas.parentElement;
    const w = parent ? parent.clientWidth : this.canvas.clientWidth;
    const h = parent ? parent.clientHeight : this.canvas.clientHeight;
    this.dpr = window.devicePixelRatio || 1;
    this.cssW = w;
    this.cssH = h;
    this.canvas.width = Math.round(w * this.dpr);
    this.canvas.height = Math.round(h * this.dpr);
    this.canvas.style.width = `${w}px`;
    this.canvas.style.height = `${h}px`;
    this.clampScroll();
    this.resizeMini();
    this.resizeCounters();
    this.invalidate();
  }

  // --- Minimap / overview strip ---------------------------------------------

  attachMinimap(canvas: HTMLCanvasElement): void {
    this.mini = canvas;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    this.miniCtx = ctx;
    canvas.addEventListener("mousedown", this.onMiniDown);
    window.addEventListener("mousemove", this.onMiniMove);
    window.addEventListener("mouseup", this.onMiniUp);
    const ro = new ResizeObserver(() => this.resizeMini());
    ro.observe(canvas.parentElement ?? canvas);
    this.resizeMini();
  }

  private resizeMini(): void {
    const c = this.mini;
    if (!c) return;
    const parent = c.parentElement;
    const w = parent ? parent.clientWidth : c.clientWidth;
    const h = parent ? parent.clientHeight : c.clientHeight;
    this.miniW = w;
    this.miniH = h;
    c.width = Math.round(w * this.dpr);
    c.height = Math.round(h * this.dpr);
    c.style.width = `${w}px`;
    c.style.height = `${h}px`;
    this.invalidate();
  }

  private computeMiniActivity(): void {
    const acc = new Array(MINI_COLS).fill(0);
    const span = this.totalSpan || 1;
    for (const s of this.slices) {
      const c = clamp(Math.floor((s.ts / span) * MINI_COLS), 0, MINI_COLS - 1);
      acc[c] += s.total;
    }
    let max = 1;
    for (const v of acc) if (v > max) max = v;
    this.miniActivity = acc;
    this.miniActivityMax = max;
  }

  // Whole-trace per-lane activity (density blocks over [0, totalSpan]) so the
  // minimap mirrors the track stack (grouping + where each process is busy).
  setOverview(blocks: DensityBlock[]): void {
    const byLane = new Map<string, Float64Array>();
    const span = this.totalSpan || 1;
    let max = 1;
    for (const b of blocks) {
      const key = `${b.pid}/${b.tid}`;
      let arr = byLane.get(key);
      if (!arr) {
        arr = new Float64Array(MINI_COLS);
        byLane.set(key, arr);
      }
      const c = clamp(Math.floor((b.ts / span) * MINI_COLS), 0, MINI_COLS - 1);
      arr[c] += b.total || b.count || 0;
      if (arr[c] > max) max = arr[c];
    }
    const ops = new Map<number, number>();
    for (const b of blocks) ops.set(b.pid, (ops.get(b.pid) ?? 0) + (b.count || 0));
    this.ovOpsByPid = ops;
    this.overviewLanes = byLane;
    this.overviewVersion++;
    this.overviewMax = max;
    this.invalidate();
  }

  // Per-process I/O busy time (us), from the proctree scan.
  setIoBusy(busyUs: Map<number, number>): void {
    this.ioBusyByPid = busyUs;
    this.invalidate();
  }

  private renderMinimap(): void {
    const ctx = this.miniCtx;
    if (!ctx) return;
    ctx.save();
    ctx.scale(this.dpr, this.dpr);
    ctx.clearRect(0, 0, this.miniW, this.miniH);
    ctx.fillStyle = this.th.plotBg;
    ctx.fillRect(0, 0, this.miniW, this.miniH);

    if (this.overviewLanes.size > 0) {
      this.renderMiniLanes(ctx);
    } else {
      const cols = this.miniActivity.length;
      const bw = this.miniW / cols;
      ctx.fillStyle = this.th.ruler;
      for (let i = 0; i < cols; i++) {
        const v = this.miniActivity[i] / this.miniActivityMax;
        if (v <= 0) continue;
        const h = Math.max(1, v * (this.miniH - 4));
        ctx.fillRect(i * bw, this.miniH - 2 - h, Math.max(1, bw), h);
      }
    }

    const x0 = (this.live.begin / this.totalSpan) * this.miniW;
    const x1 = (this.live.end / this.totalSpan) * this.miniW;
    const w = Math.max(2, x1 - x0);
    ctx.fillStyle = this.th.selFill;
    ctx.fillRect(x0, 0, w, this.miniH);
    ctx.strokeStyle = this.th.selStroke;
    ctx.lineWidth = 1;
    ctx.strokeRect(x0 + 0.5, 0.5, w - 1, this.miniH - 1);

    if (this.miniSel) {
      const sx0 = (this.miniSel.t0 / this.totalSpan) * this.miniW;
      const sx1 = (this.miniSel.t1 / this.totalSpan) * this.miniW;
      const sw = Math.max(1, sx1 - sx0);
      ctx.fillStyle = this.th.selFill;
      ctx.fillRect(sx0, 0, sw, this.miniH);
      ctx.strokeStyle = this.th.selStroke;
      ctx.strokeRect(sx0 + 0.5, 0.5, sw - 1, this.miniH - 1);
    }
    ctx.restore();
  }

  // A scaled-down heatmap of the track stack: one thin row per lane in the same
  // fork-DFS order (with process gaps), activity intensity along time.
  private renderMiniLanes(ctx: CanvasRenderingContext2D): void {
    const key = `${this.overviewVersion}|${this.procOrder.size}|${this.miniW}x${this.miniH}@${this.dpr}`;
    if (this.miniLaneKey !== key || !this.miniLaneCanvas) {
      const cv = this.miniLaneCanvas ?? document.createElement("canvas");
      cv.width = Math.max(1, Math.round(this.miniW * this.dpr));
      cv.height = Math.max(1, Math.round(this.miniH * this.dpr));
      const c2 = cv.getContext("2d");
      if (!c2) return;
      c2.setTransform(this.dpr, 0, 0, this.dpr, 0, 0);
      c2.clearRect(0, 0, this.miniW, this.miniH);
      this.paintMiniLanes(c2);
      this.miniLaneCanvas = cv;
      this.miniLaneKey = key;
    }
    ctx.drawImage(this.miniLaneCanvas, 0, 0, this.miniW, this.miniH);
  }

  private paintMiniLanes(ctx: CanvasRenderingContext2D): void {
    const orderOf = (pid: number) => this.procOrder.get(pid) ?? 1e9 + pid;
    const keys = [...this.overviewLanes.keys()].sort((a, b) => {
      const [pa, ta] = a.split("/").map(Number);
      const [pb, tb] = b.split("/").map(Number);
      return orderOf(pa) - orderOf(pb) || ta - tb;
    });
    const n = keys.length;
    if (n === 0) return;

    const top = 1;
    const bot = this.miniH - 1;
    const newProc = new Array<boolean>(n);
    let ngap = 0;
    let prevPid = "";
    for (let i = 0; i < n; i++) {
      const pid = keys[i].slice(0, keys[i].indexOf("/"));
      const isNew = i > 0 && pid !== prevPid;
      newProc[i] = isNew;
      if (isNew) ngap++;
      prevPid = pid;
    }
    const gap = n <= 48 ? 1 : 0;
    const rowH = Math.max(1, (bot - top - ngap * gap) / n);
    const bw = this.miniW / MINI_COLS;
    const denom = Math.log(this.overviewMax + 1) || 1;
    // Quantized once: a fresh rgba() string per cell means a CSS colour parse
    // per cell, which dwarfs the fills themselves.
    const shades: string[] = [];
    for (let i = 0; i <= MINI_SHADES; i++)
      shades.push(`rgba(${this.th.miniActivity},${(0.25 + (0.65 * i) / MINI_SHADES).toFixed(3)})`);

    let y = top;
    let lastShade = "";
    for (let i = 0; i < n; i++) {
      if (newProc[i]) y += gap;
      const arr = this.overviewLanes.get(keys[i]) as Float64Array;
      ctx.fillStyle = this.th.miniIdle; // idle-lane baseline
      lastShade = "";
      ctx.fillRect(0, y, this.miniW, rowH);
      for (let c = 0; c < MINI_COLS; c++) {
        const raw = arr[c];
        if (raw <= 0) continue;
        const v = Math.log(raw + 1) / denom; // log so light activity shows
        const shade = shades[Math.round(Math.min(1, v) * MINI_SHADES)];
        if (shade !== lastShade) {
          ctx.fillStyle = shade;
          lastShade = shade;
        }
        ctx.fillRect(c * bw, y, Math.max(1, bw), rowH);
      }
      y += rowH;
    }
  }

  private miniTimeAt(e: MouseEvent): number {
    if (!this.mini) return 0;
    const r = this.mini.getBoundingClientRect();
    const x = clamp(e.clientX - r.left, 0, this.miniW);
    return (x / this.miniW) * this.totalSpan;
  }

  private centerViewportAt(t: number): void {
    const span = this.target.end - this.target.begin;
    const begin = clamp(t - span / 2, this.viewLo(), Math.max(this.viewLo(), this.viewHi() - span));
    this.target = { begin, end: begin + span };
    this.invalidate();
    this.emitRange(false);
  }

  private onMiniDown = (e: MouseEvent): void => {
    if (!this.mini) return;
    e.preventDefault();
    this.miniDragging = true;
    this.miniMoved = false;
    this.miniAnchorT = this.miniTimeAt(e);
    this.miniAnchorX = e.clientX;
    this.miniSel = null;
  };

  private onMiniMove = (e: MouseEvent): void => {
    if (!this.miniDragging) return;
    if (Math.abs(e.clientX - this.miniAnchorX) > 3) this.miniMoved = true;
    if (this.miniMoved) {
      // Drag paints a selection region; releasing zooms the viewport to it.
      const t = this.miniTimeAt(e);
      this.miniSel = {
        t0: Math.min(this.miniAnchorT, t),
        t1: Math.max(this.miniAnchorT, t),
      };
      this.invalidate();
    }
  };

  private onMiniUp = (): void => {
    if (this.miniDragging && this.miniMoved && this.miniSel) {
      const { t0, t1 } = this.miniSel;
      const begin = clamp(t0, this.viewLo(), this.viewHi());
      const end = clamp(Math.max(t1, begin + MIN_SPAN), this.viewLo(), this.viewHi());
      this.target = { begin, end };
      this.invalidate();
      this.emitRange(false);
    } else if (this.miniDragging && !this.miniMoved) {
      this.centerViewportAt(this.miniAnchorT); // plain click: jump/center
    }
    this.miniDragging = false;
    this.miniSel = null;
    this.invalidate();
  };

  // --- Counter track (bandwidth) --------------------------------------------

  attachCounters(canvas: HTMLCanvasElement): void {
    this.counterCanvas = canvas;
    const ctx = canvas.getContext("2d");
    if (!ctx) return;
    this.counterCtx = ctx;
    const ro = new ResizeObserver(() => this.resizeCounters());
    ro.observe(canvas.parentElement ?? canvas);
    this.resizeCounters();
  }

  private resizeCounters(): void {
    const c = this.counterCanvas;
    if (!c) return;
    const parent = c.parentElement;
    const w = parent ? parent.clientWidth : c.clientWidth;
    const h = parent ? parent.clientHeight : c.clientHeight;
    this.counterW = w;
    this.counterH = h;
    c.width = Math.round(w * this.dpr);
    c.height = Math.round(h * this.dpr);
    c.style.width = `${w}px`;
    c.style.height = `${h}px`;
    this.invalidate();
  }

  setCounters(begin: number, bucketUs: number, read: number[], write: number[]): void {
    this.counters = { begin, bucketUs, read, write };
    const perSec = bucketUs > 0 ? 1e6 / bucketUs : 0;
    let peak = 1;
    for (let i = 0; i < read.length; i++) {
      const bw = (read[i] + write[i]) * perSec;
      if (bw > peak) peak = bw;
    }
    this.counterPeak = peak;
    this.invalidate();
  }

  private renderCounters(): void {
    const ctx = this.counterCtx;
    if (!ctx) return;
    const h = this.counterH;
    ctx.save();
    ctx.scale(this.dpr, this.dpr);
    ctx.clearRect(0, 0, this.counterW, h);
    ctx.fillStyle = this.th.plotBg;
    ctx.fillRect(0, 0, this.counterW, h);

    const d = this.counters;
    const span = this.live.end - this.live.begin;
    if (d && d.bucketUs > 0 && span > 0) {
      const perSec = 1e6 / d.bucketUs;
      const plotH = h - 4;
      ctx.save();
      ctx.beginPath();
      ctx.rect(this.gutter, 0, this.counterW - this.gutter, h);
      ctx.clip();
      // Smoothed stacked areas: draw read+write total first, then read over it,
      // so [baseline, readTop] reads teal and [readTop, totalTop] reads amber.
      const pts: { x: number; rt: number; tt: number }[] = [];
      for (let i = 0; i < d.read.length; i++) {
        const tc = d.begin + (i + 0.5) * d.bucketUs;
        const x = this.xOf(tc);
        if (x < this.gutter - 4 || x > this.counterW + 4) continue;
        const rh = ((d.read[i] * perSec) / this.counterPeak) * plotH;
        const wh = ((d.write[i] * perSec) / this.counterPeak) * plotH;
        pts.push({ x, rt: h - rh, tt: h - rh - wh });
      }
      const area = (topOf: (p: { x: number; rt: number; tt: number }) => number, color: string) => {
        if (pts.length < 2) return;
        ctx.beginPath();
        ctx.moveTo(pts[0].x, h);
        ctx.lineTo(pts[0].x, topOf(pts[0]));
        for (let i = 1; i < pts.length - 1; i++) {
          const mx = (pts[i].x + pts[i + 1].x) / 2;
          const my = (topOf(pts[i]) + topOf(pts[i + 1])) / 2;
          ctx.quadraticCurveTo(pts[i].x, topOf(pts[i]), mx, my);
        }
        const last = pts[pts.length - 1];
        ctx.lineTo(last.x, topOf(last));
        ctx.lineTo(last.x, h);
        ctx.closePath();
        ctx.fillStyle = color;
        ctx.fill();
      };
      area((p) => p.tt, this.th.write);
      area((p) => p.rt, this.th.read);
      ctx.restore();
    }

    // Left gutter: label + peak scale.
    ctx.fillStyle = this.th.plotBg;
    ctx.fillRect(0, 0, this.gutter, h);
    ctx.strokeStyle = this.th.divider;
    ctx.beginPath();
    ctx.moveTo(this.gutter - 0.5, 0);
    ctx.lineTo(this.gutter - 0.5, h);
    ctx.moveTo(0, h - 0.5);
    ctx.lineTo(this.counterW, h - 0.5);
    ctx.stroke();
    // Row 1: title (left) + peak scale (right). Row 2: read/write legend.
    ctx.fillStyle = this.th.counterLabel;
    ctx.font = '600 11px system-ui, -apple-system, "Segoe UI", sans-serif';
    ctx.textBaseline = "top";
    ctx.fillText("I/O BANDWIDTH", 8, 4);
    ctx.fillStyle = this.th.laneText;
    ctx.font = "10px ui-monospace, SFMono-Regular, Menlo, monospace";
    ctx.textAlign = "right";
    ctx.fillText(formatBytesPerSec(this.counterPeak), this.gutter - 8, 5);
    ctx.textAlign = "left";
    const ly = h - 6;
    ctx.fillStyle = this.th.read;
    ctx.fillRect(8, ly - 4, 8, 8);
    ctx.fillStyle = this.th.write;
    ctx.fillRect(58, ly - 4, 8, 8);
    ctx.fillStyle = this.th.laneText;
    ctx.textBaseline = "middle";
    ctx.fillText("read", 20, ly);
    ctx.fillText("write", 70, ly);
    ctx.restore();
  }

  // --- counter (ph="C") lanes -----------------------------------------------
  // Counters render as lanes nested in the timeline tree (see counterLanesFor):
  // node counters under the host, process counters under each process. Changing
  // the selection or mode changes which lanes exist, so both relayout.

  setCounterSelection(names: string[]): void {
    this.counterSelected = names.filter((n) => this.counterSeries.has(n));
    this.layoutLanes();
    this.invalidate();
  }

  setCounterMode(mode: "multiples" | "overlay"): void {
    if (this.counterMode === mode) return;
    this.counterMode = mode;
    this.layoutLanes();
    this.invalidate();
  }

  // Sources of a series within a lane scope: "node" = pid-0 emitters (cpu/mem),
  // a pid = that process's emitters (one line per thread).
  private scopeSources(name: string, scope: "node" | number): CounterSource[] {
    const srcs = this.counterSeries.get(name);
    if (!srcs) return [];
    return srcs.filter((s) => (scope === "node" ? s.pid === 0 : s.pid === scope));
  }

  // Per-source stats of the selected counters over a real-time range. Used by
  // the analysis panel; a source with no samples in range is omitted.
  counterRangeStats(t0: number, t1: number): CounterRangeStat[] {
    const out: CounterRangeStat[] = [];
    for (const name of this.counterSelected) {
      const srcs = this.counterSeries.get(name);
      if (!srcs) continue;
      for (const s of srcs) {
        let n = 0;
        let sum = 0;
        let mn = Infinity;
        let mx = -Infinity;
        let first = 0;
        let last = 0;
        for (let i = 0; i < s.ts.length; i++) {
          const t = s.ts[i];
          if (t < t0 || t > t1) continue;
          const v = s.val[i];
          if (n === 0) first = v;
          last = v;
          sum += v;
          n++;
          if (v < mn) mn = v;
          if (v > mx) mx = v;
        }
        if (n === 0) continue;
        out.push({
          name,
          pid: s.pid,
          tid: s.tid,
          scope: s.pid === 0 ? "node" : "proc",
          n,
          min: mn,
          max: mx,
          mean: sum / n,
          first,
          last,
        });
      }
    }
    out.sort((a, b) => a.name.localeCompare(b.name) || a.pid - b.pid || a.tid - b.tid);
    return out;
  }

  // Nearest sample of a source to real time `t` (sources are time-ordered).
  private sampleAt(s: CounterSource, t: number): { ts: number; val: number } | null {
    const ts = s.ts;
    if (ts.length === 0) return null;
    let lo = 0;
    let hi = ts.length - 1;
    while (lo < hi) {
      const mid = (lo + hi) >> 1;
      if (ts[mid] < t) lo = mid + 1;
      else hi = mid;
    }
    let best = lo;
    if (lo > 0 && Math.abs(ts[lo - 1] - t) <= Math.abs(ts[lo] - t)) best = lo - 1;
    return { ts: ts[best], val: s.val[best] };
  }

  // If (x,y) lands on a counter lane, the lane plus the nearest reading of each
  // of its series/sources to the cursor time; null otherwise.
  private counterAt(
    x: number,
    y: number,
  ): { lane: Lane; ts: number; readings: CounterReading[] } | null {
    if (x < this.gutter || y < RULER_H) return null;
    const lane = this.laneAtY(y);
    if (!lane || lane.kind !== "counter" || !lane.counter) return null;
    const t = this.toReal(this.timeOf(x));
    const readings: CounterReading[] = [];
    for (const name of lane.counter.series) {
      for (const s of this.scopeSources(name, lane.counter.scope)) {
        const smp = this.sampleAt(s, t);
        if (smp) readings.push({ name, pid: s.pid, tid: s.tid, value: smp.val, ts: smp.ts });
      }
    }
    if (readings.length === 0) return null;
    const ts = readings.reduce(
      (best, r) => (Math.abs(r.ts - t) < Math.abs(best - t) ? r.ts : best),
      readings[0].ts,
    );
    return { lane, ts, readings };
  }

  // Min/max of a series over the visible window, across all its sources. Pads a
  // flat window so a constant counter draws a centered line instead of a spike.
  private seriesExtent(srcs: CounterSource[], lo: number, hi: number): [number, number] {
    let mn = Infinity;
    let mx = -Infinity;
    for (const s of srcs) {
      for (let i = 0; i < s.ts.length; i++) {
        const t = s.ts[i];
        if (t < lo || t > hi) continue;
        const v = s.val[i];
        if (v < mn) mn = v;
        if (v > mx) mx = v;
      }
    }
    if (!Number.isFinite(mn)) return [0, 1];
    if (mn === mx) {
      const pad = Math.abs(mn) || 1;
      return [mn - pad, mx + pad];
    }
    return [mn, mx];
  }

  private drawSeriesLine(
    ctx: CanvasRenderingContext2D,
    src: CounterSource,
    mn: number,
    mx: number,
    top: number,
    plotH: number,
    color: string,
  ): void {
    const range = mx - mn || 1;
    ctx.beginPath();
    let started = false;
    for (let i = 0; i < src.ts.length; i++) {
      const x = this.xOf(src.ts[i]);
      const y = top + plotH - ((src.val[i] - mn) / range) * plotH;
      if (!started) {
        ctx.moveTo(x, y);
        started = true;
      } else {
        ctx.lineTo(x, y);
      }
    }
    if (!started) return;
    ctx.strokeStyle = color;
    ctx.lineWidth = 1.5;
    ctx.lineJoin = "round";
    ctx.stroke();
  }

  private renderCounterLanes(ctx: CanvasRenderingContext2D): void {
    if (this.live.end <= this.live.begin) return;
    for (const lane of this.lanes) {
      if (lane.kind !== "counter" || !lane.counter) continue;
      const y = RULER_H - this.scrollY + lane.y;
      const h = lane.rows * this.rowH;
      if (y + h < RULER_H || y > this.cssH) continue;
      this.drawCounterLane(ctx, lane, y, h);
    }
  }

  private drawCounterLane(ctx: CanvasRenderingContext2D, lane: Lane, y: number, h: number): void {
    const c = lane.counter!;
    const lo = this.live.begin;
    const hi = this.live.end;
    const clipTop = Math.max(y, RULER_H);
    const clipBot = Math.min(y + h, this.cssH);
    if (clipBot <= clipTop) return;
    const pad = 3;
    const plotTop = y + pad;
    const plotH = Math.max(1, h - 2 * pad);
    ctx.save();
    ctx.beginPath();
    ctx.rect(this.gutter, clipTop, this.cssW - this.gutter, clipBot - clipTop);
    ctx.clip();
    // Each series auto-scales to its own visible window; overlay lanes share the
    // lane height (comparison by shape), small-multiples lanes own it.
    for (const name of c.series) {
      const srcs = this.scopeSources(name, c.scope);
      if (!srcs.length) continue;
      const [mn, mx] = this.seriesExtent(srcs, lo, hi);
      for (const s of srcs) {
        const color =
          c.overlay || srcs.length === 1 ? colorFor(name) : colorFor(`${name}#${s.tid}`);
        this.drawSeriesLine(ctx, s, mn, mx, plotTop, plotH, color);
      }
    }
    ctx.restore();
  }

  // Gutter block for a counter lane: series name(s) + window peak, or a color
  // legend when several series share an overlay lane.
  private renderCounterGutter(
    ctx: CanvasRenderingContext2D,
    lane: Lane,
    top: number,
    bottom: number,
  ): void {
    const c = lane.counter!;
    const x = lane.indent + 4;
    ctx.save();
    ctx.beginPath();
    ctx.rect(x, top, this.gutter - 6 - x, bottom - top);
    ctx.clip();
    ctx.textBaseline = "top";
    ctx.textAlign = "left";
    if (c.overlay) {
      ctx.fillStyle = this.th.counterLabel;
      ctx.font = "600 10px ui-monospace, SFMono-Regular, Menlo, monospace";
      ctx.fillText(`${c.series.length} counters`, x, top + 2);
      ctx.font = "9px ui-monospace, SFMono-Regular, Menlo, monospace";
      let ly = top + 15;
      for (const name of c.series) {
        if (ly > bottom - 3) break;
        ctx.fillStyle = colorFor(name);
        ctx.fillRect(x, ly + 3, 10, 2);
        ctx.fillStyle = this.th.laneText;
        ctx.fillText(this.ellip(ctx, name, this.gutter - 22 - x), x + 14, ly);
        ly += 11;
      }
    } else {
      const name = c.series[0];
      ctx.fillStyle = colorFor(name);
      ctx.fillRect(x, top + 6, 8, 2);
      ctx.fillStyle = this.th.counterLabel;
      ctx.font = "600 10px ui-monospace, SFMono-Regular, Menlo, monospace";
      ctx.fillText(this.ellip(ctx, name, this.gutter - 66 - x), x + 12, top + 2);
      const [, mx] = this.seriesExtent(
        this.scopeSources(name, c.scope),
        this.live.begin,
        this.live.end,
      );
      ctx.fillStyle = this.th.numText;
      ctx.font = "9px ui-monospace, SFMono-Regular, Menlo, monospace";
      ctx.textAlign = "right";
      ctx.fillText(formatCompact(mx), this.gutter - 8, top + 2);
    }
    ctx.restore();
  }

  // Truncate `text` with an ellipsis so it fits within `maxW` px in the current
  // ctx font. Cheap linear shrink; labels are short.
  private ellip(ctx: CanvasRenderingContext2D, text: string, maxW: number): string {
    if (ctx.measureText(text).width <= maxW) return text;
    let s = text;
    while (s.length > 1 && ctx.measureText(s + "...").width > maxW) s = s.slice(0, -1);
    return s + "...";
  }

  // Render-on-demand: a frame is only scheduled while something is animating
  // (easing, held keys, pending zoom) or after an explicit invalidate(). When
  // idle, no rAF runs at all, so the app uses ~0 CPU.
  private requestFrame(): void {
    if (this.frameScheduled) return;
    this.frameScheduled = true;
    this.raf = requestAnimationFrame(this.frame);
  }

  private invalidate(): void {
    this._dirty = true;
    this.requestFrame();
  }

  // Export the timeline as a PNG blob. `whole` fits the full time range and all
  // lanes into one image (aggregated at that zoom); otherwise it captures the
  // current viewport as drawn.
  async exportPng(whole: boolean): Promise<Blob | null> {
    if (!whole) {
      return await new Promise((res) => this.canvas.toBlob((b) => res(b), "image/png"));
    }
    return this.renderWholeToBlob();
  }

  private async renderWholeToBlob(): Promise<Blob | null> {
    const MAX_SIDE = 16384; // conservative per-side canvas cap across browsers
    const saved = {
      canvas: this.canvas,
      ctx: this.ctx,
      cssW: this.cssW,
      cssH: this.cssH,
      dpr: this.dpr,
      target: this.target,
      scrollY: this.scrollY,
      rowH: this.rowH,
      hovered: this.hovered,
      hoveredGap: this.hoveredGap,
    };
    try {
      const plotW = clamp(this.totalSpan > 0 ? 3000 : this.cssW - this.gutter, 1200, 8000);
      // Fit every lane; shrink the export row height only if the full stack
      // would blow past the canvas side limit.
      if (RULER_H + this.contentH > MAX_SIDE && this.contentH > 0) {
        const factor = (MAX_SIDE - RULER_H) / this.contentH;
        this.rowH = Math.max(1, Math.floor(this.rowH * factor));
        this.layoutLanes();
      }
      const cssW = this.gutter + plotW;
      const cssH = Math.min(RULER_H + this.contentH, MAX_SIDE);
      const off = document.createElement("canvas");
      off.width = Math.round(cssW);
      off.height = Math.round(cssH);
      const octx = off.getContext("2d");
      if (!octx) return null;
      this.canvas = off;
      this.ctx = octx;
      this.dpr = 1;
      this.cssW = cssW;
      this.cssH = cssH;
      this.scrollY = 0;
      this.hovered = null;
      this.hoveredGap = null;
      this.target = { begin: 0, end: this.totalSpan || saved.target.end };
      this.render();
      return await new Promise((res) => off.toBlob((b) => res(b), "image/png"));
    } finally {
      this.canvas = saved.canvas;
      this.ctx = saved.ctx;
      this.cssW = saved.cssW;
      this.cssH = saved.cssH;
      this.dpr = saved.dpr;
      this.target = saved.target;
      this.scrollY = saved.scrollY;
      this.hovered = saved.hovered;
      this.hoveredGap = saved.hoveredGap;
      if (this.rowH !== saved.rowH) {
        this.rowH = saved.rowH;
        this.layoutLanes();
      }
      this.invalidate();
    }
  }

  private frame = (): void => {
    this.frameScheduled = false;
    this.applyKeys();
    // Apply at most one clamped zoom step per frame; carry the remainder so a
    // large momentum burst spreads across frames (smooth glide, not a jump).
    if (this.pendingZoom !== 0) {
      const step = clamp(this.pendingZoom, -ZOOM_MAX_STEP, ZOOM_MAX_STEP);
      this.zoomAt(this.zoomFocusX, step);
      this.pendingZoom -= step;
      if (Math.abs(this.pendingZoom) < 1) this.pendingZoom = 0;
    }
    const span = this.live.end - this.live.begin;
    const db = this.target.begin - this.live.begin;
    const de = this.target.end - this.live.end;
    let animating = false;
    // Relative thresholds; when both edges are within tolerance the span has
    // converged too, so no separate absolute span check is needed (an absolute
    // one never settles for microsecond-scale values and spins the loop).
    if (Math.abs(db) > span * 1e-4 || Math.abs(de) > span * 1e-4) {
      this.live.begin += db * EASE;
      this.live.end += de * EASE;
      this._dirty = true;
      animating = true;
    } else if (this.live.begin !== this.target.begin || this.live.end !== this.target.end) {
      this.live.begin = this.target.begin;
      this.live.end = this.target.end;
      this._dirty = true;
    }
    if (this._dirty) {
      this.render();
      this.renderMinimap();
      this.renderCounters();
      this._dirty = false;
    }
    if (animating || this.keys.size > 0 || this.pendingZoom !== 0) this.requestFrame();
  };

  private render(): void {
    const ctx = this.ctx;
    ctx.save();
    ctx.scale(this.dpr, this.dpr);
    ctx.clearRect(0, 0, this.cssW, this.cssH);

    ctx.fillStyle = this.th.panelBg;
    ctx.fillRect(0, 0, this.cssW, this.cssH);

    this.renderLanes(ctx);
    this.renderGridlines(ctx);
    this.renderSlices(ctx);
    this.renderCounterLanes(ctx);
    this.renderSpawnArrows(ctx);
    this.renderGaps(ctx);
    this.renderBreaks(ctx);
    this.renderSelection(ctx);
    this.renderRuler(ctx);
    this.renderGutter(ctx);
    this.renderCrosshair(ctx);

    ctx.restore();
  }

  // Spawn-connector arrows for the selected process's lineage: from the parent
  // clone to the child, drawn only on selection to avoid flow-arrow clutter.
  private renderSpawnArrows(ctx: CanvasRenderingContext2D): void {
    if (this.procParent.size === 0) return;
    // Fork arrows assume pid rows nested under hosts; skip in custom groupings.
    if (!this.isDefaultGrouping()) return;
    const laneOfPid = new Map<number, Lane>();
    for (const lane of this.lanes) {
      const p = Number(lane.pid);
      if (!laneOfPid.has(p)) laneOfPid.set(p, lane);
    }
    // Anchor at the parent lane's bottom (where it "hands off") and the child
    // lane's top (its first event), so the connector reads as parent -> child.
    const botOf = (lane: Lane) => RULER_H - this.scrollY + lane.y + lane.rows * this.rowH;
    const topOf = (lane: Lane) => RULER_H - this.scrollY + lane.y;
    const selPid = this.selected ? Number(this.selected.pid) : null;
    const inLineage = (child: number) =>
      selPid != null &&
      (child === selPid ||
        this.procParent.get(child) === selPid ||
        this.procParent.get(selPid) === child);

    // Connector from the parent's clone event to the child's first event.
    const draw = (childPid: number, hot: boolean) => {
      const parent = this.procParent.get(childPid);
      const spawn = this.procSpawn.get(childPid);
      if (parent == null || parent < 0 || spawn == null || spawn <= 0) return;
      const pl = laneOfPid.get(parent);
      const cl = laneOfPid.get(childPid);
      if (!pl || !cl) return;
      const first = this.procFirst.get(childPid) ?? spawn;
      const x1 = this.xOf(spawn);
      const x2 = this.xOf(first);
      if ((x1 < this.gutter && x2 < this.gutter) || (x1 > this.cssW && x2 > this.cssW)) return;
      const y1 = botOf(pl);
      const y2 = topOf(cl);
      ctx.strokeStyle = hot ? this.th.accent : this.th.accentSoft;
      ctx.fillStyle = ctx.strokeStyle;
      ctx.lineWidth = hot ? 1.5 : 1;
      // Near-vertical S: drop straight from the fork, ease into the child start.
      ctx.beginPath();
      ctx.moveTo(x1, y1);
      ctx.bezierCurveTo(x1, (y1 + y2) / 2, x2, (y1 + y2) / 2, x2, y2);
      ctx.stroke();
      if (hot) {
        ctx.beginPath();
        ctx.moveTo(x2, y2);
        ctx.lineTo(x2 - 4, y2 - 6);
        ctx.lineTo(x2 + 4, y2 - 6);
        ctx.closePath();
        ctx.fill();
      }
    };

    for (const [pid] of this.procParent) if (!inLineage(pid)) draw(pid, false);
    for (const [pid] of this.procParent) if (inLineage(pid)) draw(pid, true);
  }

  // Vertical cursor line plus a time readout pinned in the ruler.
  private renderCrosshair(ctx: CanvasRenderingContext2D): void {
    if (!this.cursorInside || this.dragging || this.selecting) return;
    const x = Math.round(this.mouseX) + 0.5;
    ctx.strokeStyle = this.th.crosshair;
    ctx.lineWidth = 1;
    ctx.setLineDash([3, 3]);
    ctx.beginPath();
    ctx.moveTo(x, RULER_H);
    ctx.lineTo(x, this.cssH);
    ctx.stroke();
    ctx.setLineDash([]);

    const label = formatTime(this.toReal(this.timeOf(this.mouseX)) - this.timeOrigin);
    ctx.font = "10px ui-monospace, SFMono-Regular, Menlo, monospace";
    const tw = ctx.measureText(label).width + 8;
    const lx = clamp(this.mouseX - tw / 2, this.gutter, this.cssW - tw);
    ctx.fillStyle = this.th.groupText;
    ctx.fillRect(lx, RULER_H - 15, tw, 14);
    ctx.fillStyle = this.th.plotBg;
    ctx.textBaseline = "middle";
    ctx.fillText(label, lx + 4, RULER_H - 8);
  }

  // Hatched markers at each compressed idle gap, labeled with the real time
  // skipped, so the discontinuous axis is never silent.
  private renderBreaks(ctx: CanvasRenderingContext2D): void {
    if (!this.timelapse) return;
    for (const g of this.runGaps) {
      const x0 = this.xdOf(this.toDisplay(g.begin));
      const x1 = this.xdOf(this.toDisplay(g.end));
      if (x1 < this.gutter || x0 > this.cssW) continue;
      const cx0 = Math.max(this.gutter, x0);
      const cx1 = Math.min(this.cssW, x1);
      ctx.save();
      ctx.beginPath();
      ctx.rect(cx0, RULER_H, Math.max(1, cx1 - cx0), this.cssH - RULER_H);
      ctx.clip();
      ctx.strokeStyle = this.th.grid;
      ctx.lineWidth = 1;
      ctx.beginPath();
      for (let x = cx0 - this.cssH; x < cx1 + this.cssH; x += 6) {
        ctx.moveTo(x, this.cssH);
        ctx.lineTo(x + this.cssH, RULER_H);
      }
      ctx.stroke();
      ctx.restore();
      ctx.strokeStyle = this.th.accent;
      ctx.lineWidth = 1;
      ctx.setLineDash([4, 3]);
      ctx.beginPath();
      ctx.moveTo(cx0 + 0.5, RULER_H);
      ctx.lineTo(cx0 + 0.5, this.cssH);
      ctx.moveTo(cx1 - 0.5, RULER_H);
      ctx.lineTo(cx1 - 0.5, this.cssH);
      ctx.stroke();
      ctx.setLineDash([]);
      const label = `skip ${formatTime(g.end - g.begin)}`;
      ctx.font = "9px ui-monospace, SFMono-Regular, Menlo, monospace";
      ctx.textBaseline = "middle";
      const tw = ctx.measureText(label).width + 8;
      const mid = clamp((cx0 + cx1) / 2, this.gutter + tw / 2, this.cssW - tw / 2);
      ctx.fillStyle = this.th.plotBg;
      ctx.fillRect(mid - tw / 2, RULER_H + 2, tw, 13);
      ctx.strokeStyle = this.th.accent;
      ctx.strokeRect(mid - tw / 2 + 0.5, RULER_H + 2.5, tw - 1, 12);
      ctx.fillStyle = this.th.accent;
      ctx.textAlign = "center";
      ctx.fillText(label, mid, RULER_H + 9);
      ctx.textAlign = "left";
    }
  }

  // Tick positions shared by the gridlines and the ruler labels.
  private timeTicks(): { step: number; first: number } {
    const span = this.live.end - this.live.begin;
    const step = niceStep((span / this.plotW()) * 90);
    const first = Math.ceil(this.live.begin / step) * step;
    return { step, first };
  }

  // Vertical time gridlines, drawn BEHIND the slices so event blocks sit in
  // front of them (like Perfetto).
  private renderGridlines(ctx: CanvasRenderingContext2D): void {
    const span = this.live.end - this.live.begin;
    const pw = this.plotW();
    const { step, first } = this.timeTicks();
    ctx.strokeStyle = this.th.grid;
    ctx.lineWidth = 1;
    ctx.beginPath();
    for (let t = first; t <= this.live.end; t += step) {
      const x = Math.round(this.gutter + ((t - this.live.begin) / span) * pw) + 0.5;
      ctx.moveTo(x, RULER_H);
      ctx.lineTo(x, this.cssH);
    }
    ctx.stroke();
  }

  private renderSelection(ctx: CanvasRenderingContext2D): void {
    if (!this.selection) return;
    const span = this.live.end - this.live.begin;
    const pw = this.plotW();
    const x0 = clamp(
      this.gutter + ((this.selection.t0 - this.live.begin) / span) * pw,
      this.gutter,
      this.cssW,
    );
    const x1 = clamp(
      this.gutter + ((this.selection.t1 - this.live.begin) / span) * pw,
      this.gutter,
      this.cssW,
    );
    // A rectangle selection clips to its y-band (stored in content space, so it
    // scrolls with its lanes); a plain range spans full height.
    const rect = this.selection.y0 !== undefined && this.selection.y1 !== undefined;
    const off = RULER_H - this.scrollY;
    const yTop = rect ? clamp(this.selection.y0! + off, RULER_H, this.cssH) : RULER_H;
    const yBot = rect ? clamp(this.selection.y1! + off, RULER_H, this.cssH) : this.cssH;
    ctx.fillStyle = this.th.selFill;
    ctx.fillRect(x0, yTop, x1 - x0, yBot - yTop);
    ctx.strokeStyle = this.th.selStroke;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(x0 + 0.5, yTop);
    ctx.lineTo(x0 + 0.5, yBot);
    ctx.moveTo(x1 - 0.5, yTop);
    ctx.lineTo(x1 - 0.5, yBot);
    if (rect) {
      ctx.moveTo(x0, yTop + 0.5);
      ctx.lineTo(x1, yTop + 0.5);
      ctx.moveTo(x0, yBot - 0.5);
      ctx.lineTo(x1, yBot - 0.5);
    }
    ctx.stroke();

    const rt0 = this.toReal(this.selection.t0);
    const rt1 = this.toReal(this.selection.t1);
    const dur = rt1 - rt0;
    const bw = this.selBandwidth(rt0, rt1);
    const main =
      `${formatTime(rt0 - this.timeOrigin)} → ${formatTime(rt1 - this.timeOrigin)}` +
      `   ·   Δ ${formatTime(dur)}`;
    const bwStr = bw > 0 ? `   ·   ${formatBytesPerSec(bw)}` : "";
    ctx.font = "11px ui-monospace, SFMono-Regular, Menlo, monospace";
    ctx.textBaseline = "middle";
    const pad = 8;
    const mw = ctx.measureText(main).width;
    const bwW = ctx.measureText(bwStr).width;
    const tw = mw + bwW + pad * 2;
    const lx = clamp((x0 + x1) / 2 - tw / 2, this.gutter, this.cssW - tw);
    const ty = RULER_H + 3;
    ctx.fillStyle = this.th.plotBg;
    ctx.fillRect(lx, ty, tw, 17);
    ctx.strokeStyle = this.th.accent;
    ctx.lineWidth = 1;
    ctx.strokeRect(lx + 0.5, ty + 0.5, tw - 1, 16);
    ctx.fillStyle = this.th.numText;
    ctx.fillText(main, lx + pad, ty + 9);
    ctx.fillStyle = this.th.accent;
    ctx.fillText(bwStr, lx + pad + mw, ty + 9);
  }

  // Total read+write bytes in [t0, t1] over the wall time, as bytes/sec.
  private selBandwidth(t0: number, t1: number): number {
    const c = this.counters;
    if (!c || !c.bucketUs || t1 <= t0) return 0;
    let bytes = 0;
    const n = Math.min(c.read.length, c.write.length);
    for (let i = 0; i < n; i++) {
      const bs = c.begin + i * c.bucketUs;
      const be = bs + c.bucketUs;
      if (be <= t0 || bs >= t1) continue;
      const ov = (Math.min(be, t1) - Math.max(bs, t0)) / c.bucketUs;
      bytes += ((c.read[i] || 0) + (c.write[i] || 0)) * ov;
    }
    return bytes / ((t1 - t0) / 1e6);
  }

  private renderLanes(ctx: CanvasRenderingContext2D): void {
    for (let i = 0; i < this.lanes.length; i++) {
      const lane = this.lanes[i];
      const y = RULER_H - this.scrollY + lane.y;
      const h = lane.rows * this.rowH;
      if (y + h < RULER_H || y > this.cssH) continue;
      ctx.fillStyle = i % 2 === 0 ? this.th.laneAlt : this.th.panelBg;
      ctx.fillRect(0, y, this.cssW, h);
    }
  }

  // Draw a ph=3 aggregate. Zoomed out it is one merged block; zoomed in it
  // becomes `count` uniformly-spaced synthetic marks (dashed, since the real
  // positions are unknown), degrading to a hatched band when the events
  // outnumber the pixels. Restores globalAlpha to its entry value.
  private drawAggregate(
    ctx: CanvasRenderingContext2D,
    s: Slice,
    sx: number,
    x: number,
    w: number,
    sy: number,
    pxW: number,
  ): void {
    const inset = this.rowH >= 8 ? 3 : this.rowH >= 4 ? 1 : 0;
    const y = sy + inset;
    const h = Math.max(1, this.rowH - 2 * inset);
    const baseA = ctx.globalAlpha;
    const busyFrac = s.dur > 0 ? s.total / s.dur : 1;
    const plan = planAggregate(pxW, s.count, busyFrac);

    // A filled, hatched band: the aggregate region reads as present and as an
    // estimate. Used when marks would be illegible - too dense, or so sparse
    // (deep zoom into a wide window) that none land in view.
    const drawBand = () => {
      ctx.globalAlpha = baseA * 0.3;
      ctx.fillRect(x, y, w, h);
      ctx.save();
      ctx.beginPath();
      ctx.rect(x, y, w, h);
      ctx.clip();
      ctx.strokeStyle = s.fill;
      ctx.globalAlpha = baseA * 0.6;
      ctx.lineWidth = 1;
      for (let hx = x - h; hx < x + w; hx += 5) {
        ctx.beginPath();
        ctx.moveTo(hx, y + h);
        ctx.lineTo(hx + h, y);
        ctx.stroke();
      }
      ctx.restore();
      ctx.globalAlpha = baseA;
    };

    if (plan.mode === "block") {
      ctx.globalAlpha = baseA * 0.7;
      ctx.fillRect(x, y, w, h);
      ctx.globalAlpha = baseA;
      return;
    }
    // Marks only help when at least a couple fall in the visible span; deep
    // inside a wide window they would be 0-1 lonely dots, so band instead.
    if (plan.mode === "band" || w / plan.spacing < 2) {
      drawBand();
      return;
    }
    ctx.globalAlpha = baseA * 0.14;
    ctx.fillRect(x, y, w, h);
    ctx.globalAlpha = baseA;
    ctx.setLineDash([2, 2]);
    ctx.strokeStyle = s.fill;
    ctx.lineWidth = 1;
    for (let i = 0; i < plan.n; i++) {
      const mx = sx + i * plan.spacing;
      if (mx >= this.cssW) break;
      if (mx + plan.markW <= this.gutter) continue;
      const mxx = Math.max(mx, this.gutter);
      const mw = Math.min(mx + plan.markW, this.cssW) - mxx;
      if (mw <= 0) continue;
      ctx.globalAlpha = baseA * 0.4;
      ctx.fillRect(mxx, y, mw, h);
      ctx.globalAlpha = baseA;
      ctx.strokeRect(mxx + 0.5, y + 0.5, Math.max(1, mw - 1), Math.max(1, h - 1));
    }
    ctx.setLineDash([]);
    ctx.globalAlpha = baseA;
  }

  private renderSlices(ctx: CanvasRenderingContext2D): void {
    ctx.save();
    ctx.beginPath();
    ctx.rect(this.gutter, RULER_H, this.cssW - this.gutter, this.cssH - RULER_H);
    ctx.clip();

    ctx.font = "11px ui-monospace, SFMono-Regular, Menlo, monospace";
    ctx.textBaseline = "middle";

    // Hoisted: xOf() per slice is four calls deep (toDisplay -> xdOf -> plotW)
    // and this loop runs over every slice on every frame.
    const begin = this.live.begin;
    const scale = this.plotW() / Math.max(1e-9, this.live.end - begin);
    const toDisp = this.axis.toDisplay;
    const slices = this.slices;
    const cssW = this.cssW;
    const cssH = this.cssH;
    const yTop = RULER_H - this.scrollY;
    let lastFill = "";
    let lastAlpha = 1;
    for (let si = 0; si < slices.length; si++) {
      const s = slices[si];
      const lane = this.lanes[s.laneIdx];
      const sy = yTop + lane.y + s.depth * this.rowH;
      if (sy + this.rowH < RULER_H || sy > cssH) continue;
      const sx = this.gutter + (toDisp(s.ts) - begin) * scale;
      if (sx > cssW) continue;
      const pxW = (toDisp(s.ts + s.dur) - toDisp(s.ts)) * scale;
      const sw = Math.max(this.gutter + (toDisp(s.ts + s.dur) - begin) * scale - sx, 1);
      if (sx + sw < this.gutter) continue;

      const fill = s.fill;
      const x = Math.max(sx, this.gutter);
      const w = Math.min(sx + sw, this.cssW) - x;
      // When searching, dim non-matching slices so matches stand out.
      const dim = this.searchTerm !== "" && !this.searchMatchSet.has(s);
      // Canvas state changes cost far more than the fills themselves, so only
      // touch them when the value actually changes.
      const alpha = s.density ? (dim ? 0.12 : 0.55) : s.est ? (dim ? 0.1 : 0.6) : dim ? 0.15 : 1;
      if (alpha !== lastAlpha) {
        ctx.globalAlpha = alpha;
        lastAlpha = alpha;
      }
      if (fill !== lastFill) {
        ctx.fillStyle = fill;
        lastFill = fill;
      }
      if (s.aggregated) {
        this.drawAggregate(ctx, s, sx, x, w, sy, pxW);
        lastAlpha = alpha; // drawAggregate restores globalAlpha to this
      } else if (s.density) {
        // Aggregated block: inset and dimmed so a run of them reads as a
        // "density" strip distinct from individual slices. Insets shrink on tiny
        // rows so the bar never collapses to zero height and vanishes.
        const inset = this.rowH >= 8 ? 3 : this.rowH >= 4 ? 1 : 0;
        ctx.fillRect(x, sy + inset, w, Math.max(1, this.rowH - 2 * inset));
      } else {
        const inset = this.rowH >= 4 ? 1 : 0;
        ctx.fillRect(x, sy + inset, w, Math.max(1, this.rowH - 2 * inset));
      }

      if (s.malformed) {
        if (lastAlpha !== 1) {
          ctx.globalAlpha = 1;
          lastAlpha = 1;
        }
        const my = sy + (this.rowH >= 4 ? 1 : 0);
        const mh = Math.max(1, this.rowH - (this.rowH >= 4 ? 2 : 0));
        ctx.save();
        ctx.beginPath();
        ctx.rect(x, my, w, mh);
        ctx.clip();
        ctx.strokeStyle = this.th.gapStroke;
        ctx.lineWidth = 1;
        for (let hx = x - mh; hx < x + w; hx += 5) {
          ctx.beginPath();
          ctx.moveTo(hx, my + mh);
          ctx.lineTo(hx + mh, my);
          ctx.stroke();
        }
        ctx.restore();
        ctx.strokeStyle = this.th.gapStroke;
        ctx.lineWidth = 1.5;
        ctx.strokeRect(x + 0.75, my + 0.75, w - 1.5, mh - 1.5);
        lastFill = "";
      }

      if (s === this.hovered || s.ev === this.selected || (!s.density && sw > 32)) {
        if (lastAlpha !== 1) {
          ctx.globalAlpha = 1;
          lastAlpha = 1;
        }
      }
      if (s === this.hovered) {
        ctx.strokeStyle = this.th.accent;
        ctx.lineWidth = 1;
        ctx.strokeRect(x + 0.5, sy + 1.5, w - 1, this.rowH - 3);
      }
      if (s.ev === this.selected) {
        ctx.strokeStyle = this.th.accent;
        ctx.lineWidth = 2;
        ctx.strokeRect(x + 1, sy + 2, w - 2, this.rowH - 4);
      }

      if (!s.density && !s.aggregated && sw > 32 && this.rowH >= LABEL_MIN_ROW_H) {
        ctx.fillStyle = contrastText(fill);
        lastFill = "";
        const label = String(s.ev.name ?? "");
        ctx.save();
        ctx.beginPath();
        ctx.rect(x, sy, w - 4, this.rowH);
        ctx.clip();
        ctx.fillText(label, x + 4, sy + this.rowH / 2);
        ctx.restore();
      }
    }
    ctx.restore();
  }

  private renderRuler(ctx: CanvasRenderingContext2D): void {
    ctx.fillStyle = this.th.plotBg;
    ctx.fillRect(0, 0, this.cssW, RULER_H);
    ctx.strokeStyle = this.th.divider;
    ctx.lineWidth = 1;
    ctx.beginPath();
    ctx.moveTo(0, RULER_H - 0.5);
    ctx.lineTo(this.cssW, RULER_H - 0.5);
    ctx.stroke();

    ctx.font = "9px ui-monospace, SFMono-Regular, Menlo, monospace";
    ctx.fillStyle = this.th.ruler;
    ctx.textBaseline = "middle";
    ctx.textAlign = "left";
    ctx.fillText("TRACK", 12, RULER_H / 2);
    if (this.showMetrics) {
      ctx.fillText("I/O UTIL", this.colUtil(), RULER_H / 2);
      ctx.textAlign = "right";
      ctx.fillText("OPS", this.colOps(), RULER_H / 2);
      ctx.fillText("BYTES", this.colBytes(), RULER_H / 2);
      ctx.textAlign = "left";
    }

    const span = this.live.end - this.live.begin;
    const pw = this.plotW();
    const { step, first } = this.timeTicks();
    ctx.fillStyle = this.th.laneText;
    ctx.font = "10px ui-monospace, SFMono-Regular, Menlo, monospace";
    ctx.textBaseline = "middle";
    for (let t = first; t <= this.live.end; t += step) {
      const x = this.gutter + ((t - this.live.begin) / span) * pw;
      // Short tick mark in the header; the full-height line is a background
      // gridline drawn behind the slices.
      ctx.strokeStyle = this.th.divider;
      ctx.beginPath();
      ctx.moveTo(x + 0.5, RULER_H - 6);
      ctx.lineTo(x + 0.5, RULER_H);
      ctx.stroke();
      ctx.fillText(formatTick(this.toReal(t) - this.timeOrigin, step), x + 3, RULER_H / 2);
    }
  }

  // Elbow connectors in the gutter linking each child process's accent bar up
  // to its parent (a vertical spine at the parent's indent + a horizontal tick).
  private renderGutter(ctx: CanvasRenderingContext2D): void {
    ctx.fillStyle = this.th.plotBg;
    ctx.fillRect(0, RULER_H, this.gutter, this.cssH - RULER_H);
    ctx.strokeStyle = this.th.divider;
    ctx.beginPath();
    ctx.moveTo(this.gutter - 0.5, 0);
    ctx.lineTo(this.gutter - 0.5, this.cssH);
    ctx.stroke();

    for (const lane of this.lanes) {
      const y = RULER_H - this.scrollY + lane.y;
      const h = lane.rows * this.rowH;
      const top = Math.max(y, RULER_H);
      const bottom = Math.min(y + h, this.cssH);
      if (bottom <= RULER_H || top >= this.cssH) continue;
      const cy = clamp(y + this.rowH / 2, RULER_H + this.rowH / 2, this.cssH - 2);
      // Rows too short for text: keep the structural bands (host/proc color) so
      // lanes stay distinguishable, but skip the labels and metric columns that
      // would otherwise overlap into an unreadable smear.
      const showText = this.rowH >= LABEL_MIN_ROW_H;

      if (lane.kind === "counter") {
        if (showText) this.renderCounterGutter(ctx, lane, top, bottom);
        continue;
      }

      if (lane.kind === "host") {
        ctx.fillStyle = this.th.groupBand;
        ctx.fillRect(0, top, this.gutter - 1, bottom - top);
      }

      const collapsed = this.collapsed.has(lane.collapseKey);
      if (lane.collapsible && showText) {
        ctx.fillStyle = this.th.ruler;
        ctx.font = "9px ui-monospace, SFMono-Regular, Menlo, monospace";
        ctx.textBaseline = "middle";
        ctx.textAlign = "left";
        ctx.fillText(collapsed ? ">" : "v", lane.indent + 2, cy);
      }
      if (lane.kind === "proc") {
        ctx.fillStyle = colorFor("proc:" + lane.pid);
        ctx.fillRect(lane.indent + TWIST_W - 2, top, 2, bottom - top);
      }

      // The label always draws; its font shrinks with the row so short lanes
      // keep a (tiny) label instead of vanishing. Hover surfaces it full-size.
      const labelX = lane.indent + TWIST_W + (lane.kind === "thread" ? 2 : 4);
      const labelRight = this.showMetrics ? this.colUtil() - 8 : this.gutter - 8;
      const baseFs = lane.kind === "host" || lane.kind === "proc" ? 11 : 10;
      const fs = clamp(this.rowH - 2, MIN_LABEL_FONT, baseFs);
      const weight = lane.kind === "host" ? "600 " : "";
      ctx.save();
      ctx.beginPath();
      ctx.rect(labelX, top, labelRight - labelX, bottom - top);
      ctx.clip();
      ctx.textBaseline = "middle";
      ctx.textAlign = "left";
      ctx.fillStyle =
        lane.kind === "host"
          ? this.th.groupText
          : lane.kind === "proc"
            ? this.th.laneText
            : this.th.numText;
      ctx.font = `${weight}${fs}px ui-monospace, SFMono-Regular, Menlo, monospace`;
      ctx.fillText(lane.label, labelX, cy);
      ctx.restore();

      if (lane.kind !== "thread" && this.showMetrics && showText)
        this.renderLaneColumns(ctx, lane, cy);
    }
  }

  private renderLaneColumns(ctx: CanvasRenderingContext2D, lane: Lane, cy: number): void {
    const wallUs = this.totalSpan || 1;
    const wallSec = wallUs / 1e6;
    let ops = 0;
    let bytes = 0;
    const add = (p: number) => {
      ops += this.ovOpsByPid.get(p) ?? 0;
      bytes += this.bytesByPid.get(p) ?? 0;
    };
    // Per-process I/O util; a host row averages its processes and reports the
    // spread (stdev) so cross-rank I/O imbalance is visible.
    let util = 0;
    let utilSd = 0;
    if (lane.kind === "host") {
      const utils: number[] = [];
      for (const p of this.hostPids.get(lane.host) ?? []) {
        add(p);
        utils.push(clamp((this.ioBusyByPid.get(p) ?? 0) / wallUs, 0, 1));
      }
      if (utils.length) {
        util = utils.reduce((a, b) => a + b, 0) / utils.length;
        utilSd = Math.sqrt(utils.reduce((a, b) => a + (b - util) ** 2, 0) / utils.length);
      }
    } else {
      const pid = Number(lane.pid);
      add(pid);
      util = clamp((this.ioBusyByPid.get(pid) ?? 0) / wallUs, 0, 1);
    }

    const bh = 6;
    ctx.fillStyle = this.th.divider;
    ctx.fillRect(this.colUtil(), cy - bh / 2, COL_UTIL_W, bh);
    ctx.fillStyle = util > 0.8 ? this.th.gapStroke : this.th.accent;
    ctx.fillRect(this.colUtil(), cy - bh / 2, COL_UTIL_W * util, bh);
    ctx.font = "10px ui-monospace, SFMono-Regular, Menlo, monospace";
    ctx.textBaseline = "middle";
    ctx.textAlign = "left";
    ctx.fillStyle = this.th.numText;
    const utilText =
      lane.kind === "host"
        ? `${Math.round(util * 100)}%±${Math.round(utilSd * 100)}%`
        : `${Math.round(util * 100)}%`;
    ctx.fillText(utilText, this.colUtil() + COL_UTIL_W + 5, cy);
    ctx.textAlign = "right";
    ctx.fillText(formatRate(ops / wallSec), this.colOps(), cy);
    if (bytes > 0) {
      ctx.fillText(formatBytesCompact(bytes), this.colBytes(), cy);
    } else {
      ctx.fillStyle = this.th.ruler;
      ctx.fillText("-", this.colBytes(), cy);
    }
    ctx.textAlign = "left";
  }

  // The collapsible gutter row under a gutter click, or null.
  private gutterRowAt(y: number): Lane | null {
    for (const lane of this.lanes) {
      if (!lane.collapsible) continue;
      const ly = RULER_H - this.scrollY + lane.y;
      if (y >= ly && y <= ly + this.rowH) return lane;
    }
    return null;
  }

  // Any lane (leaf or header) whose gutter band spans y - used for the
  // hidden-label hover tooltip.
  private laneAtY(y: number): Lane | null {
    for (const lane of this.lanes) {
      const ly = RULER_H - this.scrollY + lane.y;
      if (y >= ly && y <= ly + lane.rows * this.rowH) return lane;
    }
    return null;
  }
}
