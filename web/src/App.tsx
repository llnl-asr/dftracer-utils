import { createEffect, createSignal, onCleanup, onMount, For, Show } from "solid-js";
import {
  fetchCallTree,
  fetchHistogram,
  fetchInfo,
  fetchLayers,
  fetchProcTree,
  fetchViz,
  fetchVizCounters,
  fetchVizBreaks,
  fetchVizDensity,
  fetchVizStats,
  SINGLE_FILE,
} from "./data/api";
import { calleesTree, callersTree, functionList } from "./flame/sandwich";
import type {
  DensityBlock,
  FlameNode,
  HistogramResponse,
  InfoResponse,
  SelectionStats,
  TraceEvent,
  VizMetadata,
} from "./data/types";
import { CONFIG } from "./data/config";
import { onHostMessage, post } from "./data/vscode";
import { Timeline, type Gap } from "./timeline/timeline";
import { ApiExplorer } from "./api/ApiExplorer";
import { Flamegraph } from "./flame/flamegraph";
import { SandwichView } from "./flame/SandwichView";
import { FlameTooltip, type FlameHover } from "./flame/FlameTooltip";

// In the VS Code webview with no server yet: show the load screen instead of
// booting the (empty) timeline.
const NEEDS_LOAD = CONFIG.vscode && !CONFIG.apiBase;
import { colorFor, sliceKey } from "./timeline/color";
import { formatBytes, formatBytesPerSec, formatTime } from "./timeline/format";

interface HoverState {
  ev: TraceEvent;
  x: number;
  y: number;
}

interface CatStat {
  name: string;
  count: number;
}

const ICON_TIMELINE =
  '<svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor"><rect x="1.5" y="3" width="9" height="2.2" rx="1"/><rect x="1.5" y="6.9" width="13" height="2.2" rx="1"/><rect x="1.5" y="10.8" width="6" height="2.2" rx="1"/></svg>';
const ICON_FLAME =
  '<svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor"><rect x="2" y="2.5" width="12" height="2.4" rx="1"/><rect x="2" y="6.3" width="8" height="2.4" rx="1"/><rect x="2" y="10.1" width="4" height="2.4" rx="1"/></svg>';
const ICON_SANDWICH =
  '<svg viewBox="0 0 16 16" width="16" height="16" fill="currentColor"><rect x="2" y="2.6" width="12" height="2.4" rx="1" opacity=".45"/><rect x="2" y="6.3" width="12" height="2.4" rx="1"/><rect x="2" y="10" width="12" height="2.4" rx="1" opacity=".45"/></svg>';
const ICON_API =
  '<svg viewBox="0 0 16 16" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.6"><path d="M5.5 4 2 8l3.5 4M10.5 4 14 8l-3.5 4"/></svg>';

const NAV_VIEWS = [
  ["timeline", "Timeline", ICON_TIMELINE],
  ["flamegraph", "Flamegraph", ICON_FLAME],
  ["sandwich", "Sandwich", ICON_SANDWICH],
  ["api", "API", ICON_API],
] as const;

type AnGroup = "name" | "file" | "pid" | "cat" | null;
const BOTTOM_TABS: [string, string, AnGroup][] = [
  ["summary", "summary", "name"],
  ["bottlenecks", "bottlenecks", "name"],
  ["bottomup", "bottom-up", "name"],
  ["calltree", "call tree", null],
  ["ranks", "ranks", "pid"],
  ["files", "files", "file"],
  ["eventlog", "event log", null],
];

// One-line explanations shown as hover tooltips on metric labels.
const METRIC_HELP: Record<string, string> = {
  "wall time": "Elapsed time from the first to the last event in the trace.",
  "total i/o": "Total bytes read + written across all POSIX/STDIO/IO operations.",
  "i/o share": "Share of total event time spent in I/O (POSIX/STDIO/IO) categories.",
  processes: "Number of distinct processes (pids) in the trace.",
  files: "Distinct files the trace declared (FH metadata records).",
  "i/o files": "Distinct files actually read from or written to (a subset of files).",
  "i/o util":
    "Fraction of wall time a process spent in I/O operations. Host rows show mean +/- stdev across their processes.",
  ops: "Operations per second (traced events) for the process.",
  bytes: "Total bytes read + written by the process.",
  track: "Node -> process (fork tree) -> thread. Click a row to collapse/expand.",
  operation: "The traced operation (function or syscall) name.",
  layer: "I/O layer / category the operation belongs to (POSIX, MPI-IO, ...).",
  total: "Total inclusive time spent in this operation across the scope.",
  "wall share": "This operation's total time as a share of the scope's wall time.",
  self: "Time spent in this operation excluding nested child operations.",
  start: "Offset from the trace start when this event began.",
  duration: "Wall-clock time this event took.",
  size: "Bytes transferred by this I/O operation (args.ret / size).",
  bandwidth: "Bytes transferred divided by the event duration.",
  offset: "File offset of this I/O operation (hex).",
  fd: "File descriptor the operation acted on.",
  busy: "Total time of the events merged into this block.",
  events: "Number of individual events merged into this block.",
  host: "Node the process ran on (resolved from HH metadata).",
  file: "File path (resolved from FH metadata).",
  "pid/tid": "Process id / thread id.",
};

const THEME_KEY = "dftracer:theme";

// Saved choice if the user has toggled before, else the OS preference.
function initialTheme(): "dark" | "light" {
  try {
    const saved = localStorage.getItem(THEME_KEY);
    if (saved === "dark" || saved === "light") return saved;
  } catch {
    /* localStorage may be unavailable */
  }
  return typeof matchMedia === "function" && matchMedia("(prefers-color-scheme: light)").matches
    ? "light"
    : "dark";
}

// Real name -> category from /viz/layers; empty until it loads.
let LAYER_MAP: Record<string, string> = {};

// Heuristic fallback for names the summary never saw.
function layerOf(name: string): string {
  const real = LAYER_MAP[name];
  if (real) return real;
  if (/^MPI_/.test(name)) return "MPI-IO";
  if (/^(fopen|fread|fwrite|fclose|fseek|fgets|fputs|fprintf)/.test(name)) return "STDIO";
  if (
    /^(open|read|write|pread|pwrite|lseek|close|fsync|fdatasync|stat|opendir|readdir|mkdir|unlink|access|dup|fcntl|creat|truncate)/.test(
      name,
    ) ||
    /^__[a-z]*stat/.test(name) ||
    /64$/.test(name)
  )
    return "POSIX";
  return "app";
}

const ICON_GEAR =
  '<svg viewBox="0 0 24 24" width="16" height="16" fill="none" stroke="currentColor" stroke-width="1.6" stroke-linecap="round" stroke-linejoin="round"><circle cx="12" cy="12" r="3"/><path d="M19.4 15a1.65 1.65 0 0 0 .33 1.82l.06.06a2 2 0 1 1-2.83 2.83l-.06-.06a1.65 1.65 0 0 0-1.82-.33 1.65 1.65 0 0 0-1 1.51V21a2 2 0 0 1-4 0v-.09a1.65 1.65 0 0 0-1-1.51 1.65 1.65 0 0 0-1.82.33l-.06.06a2 2 0 1 1-2.83-2.83l.06-.06a1.65 1.65 0 0 0 .33-1.82 1.65 1.65 0 0 0-1.51-1H3a2 2 0 0 1 0-4h.09a1.65 1.65 0 0 0 1.51-1 1.65 1.65 0 0 0-.33-1.82l-.06-.06a2 2 0 1 1 2.83-2.83l.06.06a1.65 1.65 0 0 0 1.82.33H9a1.65 1.65 0 0 0 1-1.51V3a2 2 0 0 1 4 0v.09a1.65 1.65 0 0 0 1 1.51 1.65 1.65 0 0 0 1.82-.33l.06-.06a2 2 0 1 1 2.83 2.83l-.06.06a1.65 1.65 0 0 0-.33 1.82V9a1.65 1.65 0 0 0 1.51 1H21a2 2 0 0 1 0 4h-.09a1.65 1.65 0 0 0-1.51 1z"/></svg>';

export default function App() {
  let canvas!: HTMLCanvasElement;
  let minimap!: HTMLCanvasElement;
  let counterCanvas!: HTMLCanvasElement;
  let flameCanvas!: HTMLCanvasElement;
  let flame: Flamegraph | undefined;
  let flameInflight: AbortController | undefined;
  let flameLoadedQuery: string | null = null;
  let bottomFlame: Flamegraph | undefined;
  let timeline: Timeline | undefined;
  let inflight: AbortController | undefined;
  let counterInflight: AbortController | undefined;
  let overviewInflight: AbortController | undefined;
  // Cache the minimap overview per query so toggling a filter doesn't re-scan.
  const overviewCache = new Map<string, DensityBlock[]>();

  const [info, setInfo] = createSignal<InfoResponse | null>(null);
  const [meta, setMeta] = createSignal<VizMetadata | null>(null);
  const [loading, setLoading] = createSignal(false);
  const [error, setError] = createSignal<string | null>(null);
  const [view, setView] = createSignal<"timeline" | "flamegraph" | "sandwich" | "api">("timeline");
  const [navCollapsed, setNavCollapsed] = createSignal(false);
  const [theme, setTheme] = createSignal<"dark" | "light">(initialTheme());
  const [kpis, setKpis] = createSignal<{
    wall: number;
    totalIO: number;
    ioShare: number;
    procs: number;
  } | null>(null);

  // persist=true records an explicit user choice; the initial sync and
  // OS-preference changes leave localStorage untouched so we keep following
  // the OS until the user actually picks.
  function applyTheme(m: "dark" | "light", persist = false) {
    setTheme(m);
    document.documentElement.dataset.theme = m;
    timeline?.setTheme(m);
    flame?.setTheme(m);
    bottomFlame?.setTheme(m);
    inspCallersFg?.setTheme(m);
    inspCalleesFg?.setTheme(m);
    if (persist) {
      try {
        localStorage.setItem(THEME_KEY, m);
      } catch {
        /* ignore */
      }
    }
  }

  createEffect(() => {
    const t = anFlameTree();
    if (bottomTab() === "calltree") bottomFlame?.setTree(t);
  });
  createEffect(() => inspCallersFg?.setTree(inspCallersRoot()));
  createEffect(() => inspCalleesFg?.setTree(inspCalleesRoot()));
  const [flameLoading, setFlameLoading] = createSignal(false);
  const [flameHover, setFlameHover] = createSignal<FlameHover | null>(null);
  const [bottomFlameHover, setBottomFlameHover] = createSignal<FlameHover | null>(null);
  const [flameTree, setFlameTree] = createSignal<FlameNode | null>(null);
  const [flameTreeGrouped, setFlameTreeGrouped] = createSignal(false);
  const [byProcess, setByProcess] = createSignal(false);
  const [queryText, setQueryText] = createSignal("");
  const [appliedQuery, setAppliedQuery] = createSignal("");
  const [fullDetail, setFullDetail] = createSignal(false);
  const [selected, setSelected] = createSignal<TraceEvent | null>(null);
  const [hover, setHover] = createSignal<HoverState | null>(null);
  const [headerHelp, setHeaderHelp] = createSignal<{ text: string; x: number; y: number } | null>(
    null,
  );
  // Instant explanation tooltip for a metric label, consistent with the canvas
  // gutter headers.
  const help = (key: string) => {
    const text = METRIC_HELP[key];
    if (!text) return {};
    const show = (e: MouseEvent) => setHeaderHelp({ text, x: e.clientX, y: e.clientY });
    return { onMouseEnter: show, onMouseMove: show, onMouseLeave: () => setHeaderHelp(null) };
  };
  const [cats, setCats] = createSignal<CatStat[]>([]);
  const [showLegend, setShowLegend] = createSignal(false);
  const [searchTerm, setSearchTerm] = createSignal("");
  const [matchCount, setMatchCount] = createSignal(0);
  const [matchPos, setMatchPos] = createSignal(0);
  const [showHelp, setShowHelp] = createSignal(false);
  const [showGaps, setShowGaps] = createSignal(false);
  const [multiRun, setMultiRun] = createSignal(false);
  const [timelapse, setTimelapse] = createSignal(false);
  const [gaps, setGaps] = createSignal<Gap[]>([]);
  const [sidebarOpen, setSidebarOpen] = createSignal(false);
  const [analyzeTab, setAnalyzeTab] = createSignal<"name" | "file" | "pid" | "cat">("name");
  const [bottomTab, setBottomTab] = createSignal("summary");
  function selectBottomTab(id: string, group: AnGroup) {
    setBottomTab(id);
    if (id === "eventlog") {
      loadEventLog();
      return;
    }
    if (id === "bottlenecks" || id === "bottomup") {
      setAnalyzeTab("name"); // rows key on operation name (for the dist drill-down)
      setDistKey(null);
      setDistHist(null);
      loadAnFlame();
      return;
    }
    if (id === "calltree") {
      loadAnFlame();
      return;
    }
    if (group) loadAnalyze(group);
  }
  function reloadBottom() {
    const tab = bottomTab();
    if (tab === "eventlog") return loadEventLog();
    if (tab === "bottlenecks" || tab === "bottomup" || tab === "calltree") return loadAnFlame();
    const entry = BOTTOM_TABS.find(([bid]) => bid === tab);
    const group = entry ? entry[2] : "name";
    if (group) loadAnalyze(group);
  }
  function loadAnFlame() {
    if (totalSpan <= 0) return;
    const [b, e] = scopeRange();
    const q = appliedQuery();
    const key = `${q}|${Math.floor(b)}-${Math.ceil(e)}`;
    if (anFlameKey === key && !anFlameLoading()) return;
    anFlameInflight?.abort();
    const ac = new AbortController();
    anFlameInflight = ac;
    setAnFlameLoading(true);
    fetchCallTree(b, e, q, false, ac.signal)
      .then((res) => {
        if (ac.signal.aborted) return;
        anFlameKey = key;
        setAnFlameTree(res.tree);
      })
      .catch((err) => {
        if ((err as Error).name !== "AbortError") setError((err as Error).message);
      })
      .finally(() => {
        if (anFlameInflight === ac) {
          setAnFlameLoading(false);
          anFlameInflight = undefined;
        }
      });
  }
  const [analyzeStats, setAnalyzeStats] = createSignal<SelectionStats | null>(null);
  const [analyzeLoading, setAnalyzeLoading] = createSignal(false);
  let analyzeInflight: AbortController | undefined;
  const [eventLog, setEventLog] = createSignal<TraceEvent[] | null>(null);
  const [eventLogLoading, setEventLogLoading] = createSignal(false);
  const [eventLogTruncated, setEventLogTruncated] = createSignal(false);
  let eventLogInflight: AbortController | undefined;
  // Separate from the Flamegraph view's tree so the two never clobber.
  const [anFlameTree, setAnFlameTree] = createSignal<FlameNode | null>(null);
  const [anFlameLoading, setAnFlameLoading] = createSignal(false);
  let anFlameInflight: AbortController | undefined;
  let anFlameKey: string | null = null;
  const isFlameTab = () => bottomTab() === "bottlenecks" || bottomTab() === "bottomup";
  // bottlenecks ranks by inclusive time, bottom-up by self (exclusive) time.
  const bottomFns = () => {
    const t = anFlameTree();
    if (!t) return [];
    const list = functionList([t]);
    return bottomTab() === "bottomup"
      ? list.sort((a, b) => b.self - a.self)
      : list.sort((a, b) => b.total - a.total);
  };
  // Cache Analyze results per (query, tab); the whole-trace scan is expensive.
  const analyzeCache = new Map<string, SelectionStats>();
  const [distKey, setDistKey] = createSignal<string | null>(null);
  const [distHist, setDistHist] = createSignal<HistogramResponse | null>(null);
  const [distLoading, setDistLoading] = createSignal(false);
  let distInflight: AbortController | undefined;
  const [inspHist, setInspHist] = createSignal<HistogramResponse | null>(null);
  let inspInflight: AbortController | undefined;
  const [inspSwName, setInspSwName] = createSignal<string | null>(null);
  const [inspSwTree, setInspSwTree] = createSignal<FlameNode | null>(null);
  const [inspSwHover, setInspSwHover] = createSignal<FlameHover | null>(null);
  let inspCallersFg: Flamegraph | undefined;
  let inspCalleesFg: Flamegraph | undefined;
  const inspCallersRoot = () => {
    const t = inspSwTree();
    const n = inspSwName();
    return t && n ? callersTree([t], n) : null;
  };
  const inspCalleesRoot = () => {
    const t = inspSwTree();
    const n = inspSwName();
    return t && n ? calleesTree([t], n) : null;
  };
  // Cached per query, shared by every selection's sandwich to avoid refetching.
  let inspTreeQuery: string | null = null;
  let inspTreePromise: Promise<FlameNode | null> | null = null;
  let searchInput: HTMLInputElement | undefined;
  // Resolution maps accumulated from HH/FH metadata (persist across fetches;
  // metadata has no ts so it only arrives on the full-range load).
  const [hostByHash, setHostByHash] = createSignal<Map<string, string>>(new Map());
  const [fileByHash, setFileByHash] = createSignal<Map<string, string>>(new Map());
  const hostByPid = new Map<string, string>();
  // Range the Analyze panel aggregates over; null means the whole trace.
  const [anScope, setAnScope] = createSignal<{ t0: number; t1: number } | null>(null);
  const scopeRange = (): [number, number] => {
    const s = anScope();
    return s ? [s.t0, s.t1] : [0, totalSpan];
  };
  const scopeKey = () => {
    const s = anScope();
    return s ? `${Math.floor(s.t0)}-${Math.ceil(s.t1)}` : "all";
  };

  const [procRank, setProcRank] = createSignal<Map<string, string>>(new Map());
  const [fileCounts, setFileCounts] = createSignal<{ total: number; io: number } | null>(null);
  const [showSettings, setShowSettings] = createSignal(false);
  const [serverPath, setServerPath] = createSignal("");

  const summary = () => (fullDetail() ? 1 : 2);

  // Overscan buffer: the timeline holds slices for a window wider than the
  // viewport, so zooming/panning inside it needs no refetch (no data pop). We
  // only refetch when the viewport leaves the buffer, zooms in far enough that
  // the buffer is coarse, changes LOD, or the query changes.
  let totalSpan = 1;
  let loaded = { begin: 0, end: 0, summary: -1 };
  // Longest event duration seen so far; sent as `lookback` so the server also
  // returns events that started before the window but extend into it, without
  // scanning back to the start of the trace.
  let maxDurSeen = 0;
  let didAutoFit = false;

  function ensureData(begin: number, end: number, force = false) {
    const s = summary();
    const span = Math.max(1, end - begin);
    const bufferSpan = loaded.end - loaded.begin;
    const insideBuffer =
      !force &&
      s === loaded.summary &&
      begin >= loaded.begin &&
      end <= loaded.end &&
      bufferSpan <= span * 4; // buffer not so wide the detail is stale
    if (insideBuffer) return;
    const margin = span; // 1x overscan each side (3x window)
    const fb = Math.max(0, begin - margin);
    const fe = Math.min(totalSpan, end + margin);
    loaded = { begin: fb, end: fe, summary: s };
    requestData(fb, fe);
  }

  // Whole-trace per-lane overview for the minimap heatmap. Coarse (served from
  // the mipmap when unfiltered) and re-fetched whenever the query changes so the
  // minimap matches the filtered tracks. Best-effort; supersedes in flight.
  function loadOverview() {
    if (totalSpan <= 0) return;
    overviewInflight?.abort();
    const key = appliedQuery();
    const cached = overviewCache.get(key);
    if (cached) {
      timeline?.setOverview(cached);
      return;
    }
    const ac = new AbortController();
    overviewInflight = ac;
    fetchVizDensity(
      { begin: 0, end: totalSpan, summary: 2, query: key, width: timeline?.viewportWidth() },
      ac.signal,
    )
      .then((res) => {
        if (ac.signal.aborted) return;
        overviewCache.set(key, res.density);
        timeline?.setOverview(res.density);
      })
      .catch(() => {});
  }

  // Whole-trace call tree for the flamegraph, respecting the applied query.
  // Fetched on entering the tab or when the query changes; skipped if already
  // showing the current query.
  // The sandwich always fetches the grouped tree (so it can scope to a process
  // client-side); the flamegraph groups only when its toggle is on.
  const flameGrouped = () => (view() === "sandwich" ? true : byProcess());

  function loadFlame() {
    if (totalSpan <= 0) return;
    const q = appliedQuery();
    const grouped = flameGrouped();
    const key = `${q}|${grouped ? "p" : ""}`;
    if (flameLoadedQuery === key && !flameLoading()) return;
    flameInflight?.abort();
    const ac = new AbortController();
    flameInflight = ac;
    setFlameLoading(true);
    fetchCallTree(0, totalSpan, q, grouped, ac.signal)
      .then((res) => {
        if (ac.signal.aborted) return;
        flameLoadedQuery = key;
        setFlameTreeGrouped(grouped);
        setFlameTree(res.tree);
        flame?.setTree(res.tree);
      })
      .catch((err) => {
        if ((err as Error).name !== "AbortError") setError((err as Error).message);
      })
      .finally(() => {
        if (flameInflight === ac) {
          setFlameLoading(false);
          flameInflight = undefined;
        }
      });
  }

  async function loadKpis(procs: number) {
    let totalIO = 0;
    let ioShare = 0;
    try {
      const [c, s] = await Promise.all([
        fetchVizCounters(0, totalSpan, "", 400),
        fetchVizStats(0, totalSpan, "", "cat"),
      ]);
      const sum = (a: number[]) => a.reduce((x, y) => x + y, 0);
      totalIO = sum(c.read_bytes) + sum(c.write_bytes);
      const io = new Set(["POSIX", "STDIO", "IO"]);
      let ioT = 0;
      let allT = 0;
      for (const n of s.names) {
        allT += n.total;
        if (io.has(n.name)) ioT += n.total;
      }
      ioShare = allT > 0 ? (ioT / allT) * 100 : 0;
    } catch {
      /* best-effort */
    }
    setKpis({ wall: totalSpan, totalIO, ioShare, procs });
  }

  function loadInspHist(name: string) {
    inspInflight?.abort();
    const ac = new AbortController();
    inspInflight = ac;
    setInspHist(null);
    const esc = name.replace(/\\/g, "\\\\").replace(/"/g, '\\"');
    const q = appliedQuery();
    const pred = `name == "${esc}"`;
    fetchHistogram(0, totalSpan, q ? `(${q}) and ${pred}` : pred, ac.signal)
      .then((r) => {
        if (!ac.signal.aborted) setInspHist(r);
      })
      .catch(() => {});
  }

  function ensureInspTree(): Promise<FlameNode | null> {
    const q = appliedQuery();
    if (inspTreeQuery === q && inspTreePromise) return inspTreePromise;
    inspTreeQuery = q;
    inspTreePromise = fetchCallTree(0, totalSpan, q, false)
      .then((r) => r.tree)
      .catch(() => null);
    return inspTreePromise;
  }

  async function loadInspSandwich(name: string) {
    setInspSwName(name);
    setInspSwTree(null);
    const tree = await ensureInspTree();
    if (inspSwName() !== name) return; // superseded by a newer selection
    setInspSwTree(tree);
  }

  function inspRows(ev: TraceEvent): [string, string][] {
    if (isAggregated(ev)) {
      return [
        ["events", Number(ev.count ?? 0).toLocaleString()],
        ["busy", formatTime(Number(ev.total ?? 0))],
        ["pid/tid", `${String(ev.pid)}/${String(ev.tid)}`],
        ["start", formatTime(Number(ev.ts) || 0)],
      ];
    }
    const args = (ev.args ?? {}) as Record<string, unknown>;
    const dur = Number(ev.dur) || 0;
    const rows: [string, string][] = [
      ["layer", String(ev.cat ?? "-")],
      ["start", formatTime(Number(ev.ts) || 0)],
      ["duration", formatTime(dur)],
    ];
    const size = Number(args.size ?? args.ret ?? args.image_size);
    if (Number.isFinite(size) && size > 0) {
      rows.push(["size", formatBytes(size)]);
      if (dur > 0) rows.push(["bandwidth", formatBytesPerSec(size / (dur / 1e6))]);
    }
    if (args.offset != null) {
      const off = Number(args.offset);
      rows.push([
        "offset",
        Number.isFinite(off) ? "0x" + off.toString(16).toUpperCase() : String(args.offset),
      ]);
    }
    if (args.fd != null) rows.push(["fd", String(args.fd)]);
    return rows;
  }

  function resolvedRows(ev: TraceEvent): [string, string][] {
    const args = (ev.args ?? {}) as Record<string, unknown>;
    const out: [string, string][] = [];
    if (args.hhash != null) {
      const host = hostByHash().get(String(args.hhash));
      if (host) out.push(["host", host]);
    }
    if (args.fhash != null) {
      const file = fileByHash().get(String(args.fhash));
      if (file) out.push(["file", file]);
    }
    return out;
  }

  function showView(v: "timeline" | "flamegraph" | "sandwich" | "api") {
    setView(v);
    if (v === "flamegraph" || v === "sandwich") loadFlame();
  }

  async function requestData(begin: number, end: number) {
    inflight?.abort();
    const ac = new AbortController();
    inflight = ac;
    setLoading(true);
    setError(null);
    try {
      const res = await fetchVizDensity(
        {
          begin,
          end,
          summary: summary(),
          query: appliedQuery(),
          lookback: maxDurSeen,
          width: timeline?.viewportWidth(),
        },
        ac.signal,
      );
      if (ac.signal.aborted) return;
      setMeta(res.metadata);
      timeline?.setData(res.events, res.density);

      // Single-file view: the file's data may be a tiny sliver of the global
      // (multi-node) span, so fit the viewport to it on first load.
      if (SINGLE_FILE && !didAutoFit) {
        let lo = Infinity;
        let hi = -Infinity;
        for (const e of res.events) {
          const t = Number(e.ts);
          if (Number.isFinite(t)) {
            lo = Math.min(lo, t);
            hi = Math.max(hi, t + (Number(e.dur) || 0));
          }
        }
        for (const b of res.density) {
          lo = Math.min(lo, b.ts);
          hi = Math.max(hi, b.ts + (Number(b.dur) || 0));
        }
        if (Number.isFinite(lo) && hi > lo && hi - lo < totalSpan * 0.9) {
          didAutoFit = true;
          timeline?.focusRange(lo, hi);
        }
      }
      // The server reports the longest event it scanned (including ones folded
      // into density blocks), so lookback converges even when big enclosing
      // events never come back as individual slices.
      const md = Number(res.metadata.max_dur);
      if (Number.isFinite(md) && md > maxDurSeen) maxDurSeen = md;

      // Counter track (bandwidth) for the same window; fire-and-forget so it
      // never blocks the slice data.
      counterInflight?.abort();
      const cac = new AbortController();
      counterInflight = cac;
      fetchVizCounters(begin, end, appliedQuery(), 1200, cac.signal)
        .then((c) => {
          if (!cac.signal.aborted)
            timeline?.setCounters(c.begin, c.bucket_us, c.read_bytes, c.write_bytes);
        })
        .catch(() => {});

      const counts = new Map<string, number>();
      for (const b of res.density) {
        if (b.name) counts.set(b.name, (counts.get(b.name) ?? 0) + b.count);
      }
      const hh = new Map(hostByHash());
      const fh = new Map(fileByHash());
      let resolvedChanged = false;
      for (const ev of res.events) {
        const args = (ev.args ?? {}) as Record<string, unknown>;
        if (ev.ph === "M") {
          const val = args.value != null ? String(args.value) : "";
          const nm = args.name != null ? String(args.name) : "";
          if (ev.name === "HH" && val && nm && hh.get(val) !== nm) {
            hh.set(val, nm);
            resolvedChanged = true;
          } else if (ev.name === "FH" && val && nm && fh.get(val) !== nm) {
            fh.set(val, nm);
            resolvedChanged = true;
          }
          continue;
        }
        const c = sliceKey(ev);
        if (c) counts.set(c, (counts.get(c) ?? 0) + 1);
        if (args.hhash != null && !hostByPid.has(String(ev.pid))) {
          hostByPid.set(String(ev.pid), String(args.hhash));
        }
      }
      if (resolvedChanged) {
        setHostByHash(hh);
        setFileByHash(fh);
      }
      setCats(
        [...counts.entries()]
          .map(([name, count]) => ({ name, count }))
          .sort((a, b) => b.count - a.count),
      );

      const hosts = new Map<number, string>();
      for (const [pid, hash] of hostByPid) {
        const host = hh.get(hash);
        if (host) hosts.set(Number(pid), host);
      }
      timeline?.setHosts(hosts);
      setGaps(timeline?.topGaps(15) ?? []);
    } catch (err) {
      if ((err as Error).name === "AbortError") return;
      loaded.summary = -1; // invalidate buffer so we retry on next change
      setError((err as Error).message);
    } finally {
      if (inflight === ac) {
        setLoading(false);
        inflight = undefined;
      }
    }
  }

  // Server-side per-name aggregation for the selected range.
  function requestSelection(t0: number, t1: number) {
    setSelected(null);
    setAnScope({ t0, t1 });
    setSidebarOpen(true);
    reloadBottom();
  }

  function clearSelection() {
    if (anScope() === null) return;
    setAnScope(null);
    timeline?.clearSelection();
    if (sidebarOpen()) reloadBottom();
  }

  function afterQueryChange() {
    loadOverview();
    if (view() === "flamegraph" || view() === "sandwich") loadFlame();
    const vp = timeline?.getViewport();
    if (vp) ensureData(vp.begin, vp.end, true);
  }

  function applyQuery(e?: Event) {
    e?.preventDefault();
    setAppliedQuery(queryText());
    afterQueryChange();
  }

  function filterByName(name: string) {
    // The query DSL has no string escapes, so such a name cannot be expressed.
    if (name.includes('"') || name.includes("\\")) {
      setError(`Cannot filter on a name containing a quote or backslash: ${name}`);
      return;
    }
    const q = `name == "${name}"`;
    setQueryText(q);
    setAppliedQuery(q);
    afterQueryChange();
  }

  function clearQuery() {
    setQueryText("");
    setAppliedQuery("");
    afterQueryChange();
  }

  function doSearch(term: string) {
    setSearchTerm(term);
    setMatchCount(timeline?.setSearch(term) ?? 0);
    setMatchPos(0);
  }

  function navMatch(dir: 1 | -1) {
    if (matchCount() > 0) setMatchPos(timeline?.focusMatch(dir) ?? 0);
  }

  async function loadAnalyze(tab: "name" | "file" | "pid" | "cat") {
    setAnalyzeTab(tab);
    setDistKey(null);
    setDistHist(null);
    analyzeInflight?.abort();
    const cacheKey = `${appliedQuery()}|${tab}|${scopeKey()}`;
    const cached = analyzeCache.get(cacheKey);
    if (cached) {
      setAnalyzeStats(cached);
      setAnalyzeLoading(false);
      return;
    }
    const ac = new AbortController();
    analyzeInflight = ac;
    setAnalyzeLoading(true);
    try {
      const group = tab === "file" ? "fhash" : tab;
      const [b, e] = scopeRange();
      const res = await fetchVizStats(b, e, appliedQuery(), group, ac.signal);
      if (!ac.signal.aborted) {
        analyzeCache.set(cacheKey, res);
        setAnalyzeStats(res);
      }
    } catch (err) {
      if ((err as Error).name !== "AbortError") setError((err as Error).message);
    } finally {
      if (analyzeInflight === ac) {
        setAnalyzeLoading(false);
        analyzeInflight = undefined;
      }
    }
  }

  async function loadEventLog() {
    eventLogInflight?.abort();
    const ac = new AbortController();
    eventLogInflight = ac;
    setEventLogLoading(true);
    try {
      const [b, e] = scopeRange();
      const res = await fetchViz(
        { begin: b, end: e, summary: 1, query: appliedQuery(), limit: 1000 },
        ac.signal,
      );
      if (!ac.signal.aborted) {
        const evs = res.events.filter((e) => e.ph !== "M");
        evs.sort((a, b) => Number(a.ts) - Number(b.ts));
        setEventLog(evs);
        setEventLogTruncated(res.metadata.truncated);
      }
    } catch (err) {
      if ((err as Error).name !== "AbortError") setError((err as Error).message);
    } finally {
      if (eventLogInflight === ac) {
        setEventLogLoading(false);
        eventLogInflight = undefined;
      }
    }
  }

  function analyzeLabel(raw: string): string {
    if (analyzeTab() === "file") return fileByHash().get(raw) ?? raw;
    if (analyzeTab() === "pid") {
      const host = hostByHash().get(hostByPid.get(raw) ?? "");
      return host ? `${host} / P${raw}` : `Process ${raw}`;
    }
    return raw;
  }

  // Query predicate narrowing to one Analyze row (null when not expressible,
  // e.g. the file tab keys on a resolved path, not the fhash we could filter on).
  function distPredicate(tab: string, raw: string): string | null {
    const esc = (s: string) => s.replace(/\\/g, "\\\\").replace(/"/g, '\\"');
    if (tab === "name") return `name == "${esc(raw)}"`;
    if (tab === "cat") return `cat == "${esc(raw)}"`;
    if (tab === "pid") return `pid == ${raw}`;
    return null;
  }

  async function loadDist(raw: string) {
    const pred = distPredicate(analyzeTab(), raw);
    if (!pred) return;
    setDistKey(raw);
    distInflight?.abort();
    const ac = new AbortController();
    distInflight = ac;
    setDistLoading(true);
    setDistHist(null);
    const q = appliedQuery();
    const combined = q ? `(${q}) and ${pred}` : pred;
    try {
      const [b, e] = scopeRange();
      const res = await fetchHistogram(b, e, combined, ac.signal);
      if (!ac.signal.aborted) setDistHist(res);
    } catch (err) {
      if ((err as Error).name !== "AbortError") setError((err as Error).message);
    } finally {
      if (distInflight === ac) {
        setDistLoading(false);
        distInflight = undefined;
      }
    }
  }

  function onSearchKey(e: KeyboardEvent) {
    if (e.key === "Enter") {
      e.preventDefault();
      navMatch(e.shiftKey ? -1 : 1);
    } else if (e.key === "Escape") {
      doSearch("");
      searchInput?.blur();
    }
  }

  onMount(async () => {
    timeline = new Timeline(canvas, {
      onRangeChange: (b, e) => ensureData(b, e),
      onHover: (ev, x, y) => setHover(ev ? { ev, x, y } : null),
      onHeaderHover: (key, x, y) => {
        const text = key ? METRIC_HELP[key] : undefined;
        setHeaderHelp(text ? { text, x, y } : null);
      },
      onSelect: (ev) => {
        setSelected(ev);
        if (ev) {
          clearSelection();
          if (ev.name != null) {
            loadInspHist(String(ev.name));
            loadInspSandwich(String(ev.name));
          }
        } else {
          setInspSwName(null);
          setInspSwTree(null);
        }
      },
      onSelectRange: (t0, t1) => requestSelection(t0, t1),
      onSelectRangeClear: () => clearSelection(),
    });
    timeline.attachMinimap(minimap);
    timeline.attachCounters(counterCanvas);
    flame = new Flamegraph(flameCanvas, {
      onHover: (node, pct, x, y) => setFlameHover(node ? { node, pct, x, y } : null),
    });
    window.addEventListener("keydown", onGlobalKey);
    if (CONFIG.vscode) {
      onHostMessage((m) => {
        if (m.type === "serverPath" && typeof m.path === "string") setServerPath(m.path);
      });
      post({ type: "getServerPath" });
    }
    // Sync the initial theme (OS preference or saved choice) into the canvases.
    applyTheme(theme());
    // Follow OS changes until the user makes an explicit choice.
    if (typeof matchMedia === "function") {
      const mq = matchMedia("(prefers-color-scheme: light)");
      mq.addEventListener?.("change", (e) => {
        let saved: string | null = null;
        try {
          saved = localStorage.getItem(THEME_KEY);
        } catch {
          /* ignore */
        }
        if (saved !== "dark" && saved !== "light") applyTheme(e.matches ? "light" : "dark");
      });
    }
    if (NEEDS_LOAD) return; // no server yet; the load screen is shown instead
    try {
      const i = await fetchInfo();
      setInfo(i);
      const tr = i.time_range;
      const span = tr ? tr.max_timestamp_us - tr.min_timestamp_us : 0;
      if (span > 0) {
        totalSpan = span;
        timeline.setTotalSpan(span);
        fetchVizBreaks()
          .then((br) => {
            setMultiRun(br.multi_run);
            setTimelapse(br.multi_run);
            timeline?.setBreaks(br.gaps, br.multi_run);
          })
          .catch(() => {});
        loadOverview();
      } else {
        setError("No indexed events with a valid time range were found.");
      }
      // Retry once if empty: the first request may race the summary build.
      const loadLayers = async (retry: boolean) => {
        try {
          const r = await fetchLayers();
          setFileCounts({ total: r.total_files, io: r.io_files });
          if (Object.keys(r.layers).length) LAYER_MAP = r.layers;
          else if (retry) setTimeout(() => loadLayers(false), 1500);
        } catch {
          /* heuristic fallback */
        }
      };
      loadLayers(true);
      // Fork hierarchy for lane ordering + spawn arrows; best-effort.
      fetchProcTree()
        .then((pt) => {
          timeline?.setProcTree(pt.nodes);
          const hosts = new Map<number, string>();
          const bytes = new Map<number, number>();
          const ioBusy = new Map<number, number>();
          const labels = new Map<string, string>();
          const ranks = new Map<string, string>();
          for (const n of pt.nodes) {
            if (n.host) hosts.set(n.pid, n.host);
            if (n.bytes) bytes.set(n.pid, n.bytes);
            if (n.io_busy) ioBusy.set(n.pid, n.io_busy);
            const hasRank = n.rank != null && n.rank !== "";
            labels.set(String(n.pid), hasRank ? `rank ${n.rank}` : `proc ${n.pid}`);
            if (hasRank) ranks.set(String(n.pid), n.rank as string);
          }
          timeline?.setHosts(hosts);
          timeline?.setBytes(bytes);
          timeline?.setIoBusy(ioBusy);
          timeline?.setProcessLabels(labels);
          setProcRank(ranks);
          setGaps(timeline?.topGaps(15) ?? []); // relabel with resolved ranks
          loadKpis(new Set(pt.nodes.map((n) => n.pid)).size);
        })
        .catch(() => loadKpis(0));
    } catch (err) {
      setError(`Failed to load /api/v1/info: ${(err as Error).message}`);
    }
  });

  function onGlobalKey(e: KeyboardEvent) {
    const el = document.activeElement;
    const typing = el && (el.tagName === "INPUT" || el.tagName === "TEXTAREA");
    if ((e.ctrlKey || e.metaKey) && e.key.toLowerCase() === "f") {
      e.preventDefault();
      searchInput?.focus();
      searchInput?.select();
    } else if (e.key === "?" && !typing) {
      e.preventDefault();
      setShowHelp((v) => !v);
    } else if (e.key === "Escape" && showHelp()) {
      setShowHelp(false);
    } else if (e.key === "Escape" && anScope()) {
      clearSelection();
    }
  }

  onCleanup(() => {
    inflight?.abort();
    counterInflight?.abort();
    analyzeInflight?.abort();
    flameInflight?.abort();
    anFlameInflight?.abort();
    window.removeEventListener("keydown", onGlobalKey);
    timeline?.destroy();
    flame?.destroy();
    bottomFlame?.destroy();
    inspCallersFg?.destroy();
    inspCalleesFg?.destroy();
  });

  return (
    <div class="app">
      <Show when={headerHelp()}>
        {(h) => (
          <div class="tooltip help-tip" style={{ left: `${h().x + 14}px`, top: `${h().y + 18}px` }}>
            {h().text}
          </div>
        )}
      </Show>
      <nav class="leftnav" classList={{ collapsed: navCollapsed() }}>
        <div class="nav-top">
          <div class="brand" title="DFTracer">
            <span class="brand-mark" />
            <span class="brand-text">DFTracer</span>
          </div>
          <button
            type="button"
            class="nav-collapse"
            title={navCollapsed() ? "Expand sidebar" : "Collapse sidebar"}
            onClick={() => setNavCollapsed(!navCollapsed())}
          >
            {navCollapsed() ? ">" : "<"}
          </button>
        </div>
        <div class="nav-views">
          <For each={NAV_VIEWS}>
            {([id, label, icon]) => (
              <button
                type="button"
                class="nav-item"
                classList={{ active: view() === id }}
                title={label}
                onClick={() => showView(id)}
              >
                <span class="nav-icon" innerHTML={icon} />
                <span class="nav-label">{label}</span>
              </button>
            )}
          </For>
        </div>
        <div class="nav-bottom">
          <button
            type="button"
            class="nav-item"
            classList={{ active: showSettings() }}
            title="Settings"
            onClick={() => setShowSettings((v) => !v)}
          >
            <span class="nav-icon" innerHTML={ICON_GEAR} />
            <span class="nav-label">Settings</span>
          </button>
        </div>
      </nav>
      <div class="main">
        <Show when={loading()}>
          <div class="loadbar" />
        </Show>
        <header class="toolbar">
          <form class="query" onSubmit={applyQuery}>
            <input
              type="text"
              placeholder='query, e.g.  dur >= 1000 and cat == "POSIX"'
              value={queryText()}
              onInput={(e) => setQueryText(e.currentTarget.value)}
              spellcheck={false}
            />
            <button type="submit">Apply</button>
            <Show when={appliedQuery()}>
              <button type="button" class="ghost" onClick={clearQuery}>
                Clear
              </button>
            </Show>
          </form>
          <Show when={view() === "timeline"}>
            <div class="search">
              <input
                ref={searchInput}
                type="text"
                placeholder="find (Ctrl+F)"
                value={searchTerm()}
                onInput={(e) => doSearch(e.currentTarget.value)}
                onKeyDown={onSearchKey}
                spellcheck={false}
              />
              <Show when={searchTerm()}>
                <span class="match-count">
                  {matchCount()
                    ? matchPos()
                      ? `${matchPos()}/${matchCount()}`
                      : String(matchCount())
                    : "0"}
                </span>
                <button
                  type="button"
                  class="ghost sm"
                  title="previous (Shift+Enter)"
                  onClick={() => navMatch(-1)}
                >
                  {"<"}
                </button>
                <button
                  type="button"
                  class="ghost sm"
                  title="next (Enter)"
                  onClick={() => navMatch(1)}
                >
                  {">"}
                </button>
              </Show>
            </div>
            <label class="toggle">
              <input
                type="checkbox"
                checked={fullDetail()}
                onChange={(e) => {
                  setFullDetail(e.currentTarget.checked);
                  const vp = timeline?.getViewport();
                  if (vp) ensureData(vp.begin, vp.end, true);
                }}
              />
              Full detail
            </label>
            <button
              type="button"
              class="ghost"
              classList={{ active: showLegend() }}
              onClick={() => setShowLegend(!showLegend())}
            >
              Legend
            </button>
            <button
              type="button"
              class="ghost"
              classList={{ active: showGaps() }}
              onClick={() => {
                const v = !showGaps();
                setShowGaps(v);
                timeline?.setShowGaps(v);
              }}
            >
              Gaps
            </button>
            <Show when={multiRun()}>
              <button
                type="button"
                class="ghost"
                classList={{ active: timelapse() }}
                title="Compress the dead time between separate runs"
                onClick={() => {
                  const v = !timelapse();
                  setTimelapse(v);
                  timeline?.setTimelapse(v);
                }}
              >
                Timelapse
              </button>
            </Show>
          </Show>
          <Show when={view() === "flamegraph"}>
            <button
              type="button"
              class="ghost"
              classList={{ active: byProcess() }}
              title="Split the tree by process instead of merging all processes"
              onClick={() => {
                setByProcess(!byProcess());
                loadFlame();
              }}
            >
              Group by process
            </button>
          </Show>
          <button
            type="button"
            class="ghost"
            classList={{ active: sidebarOpen() }}
            onClick={() => {
              const open = !sidebarOpen();
              setSidebarOpen(open);
              if (open) loadAnalyze(analyzeTab());
            }}
          >
            Analyze
          </button>
          <Show when={view() !== "sandwich"}>
            <button
              type="button"
              class="ghost"
              onClick={() =>
                view() === "flamegraph" ? flame?.resetFocus() : timeline?.resetView()
              }
            >
              Reset
            </button>
          </Show>
          <button
            type="button"
            class="ghost"
            title="shortcuts (?)"
            onClick={() => setShowHelp(true)}
          >
            ?
          </button>
        </header>

        <Show when={kpis()}>
          {(k) => (
            <div class="kpi-strip">
              <div class="kpi">
                <span {...help("wall time")}>wall time</span>
                <b>{formatTime(k().wall)}</b>
              </div>
              <div class="kpi">
                <span {...help("total i/o")}>total i/o</span>
                <b>{formatBytes(k().totalIO)}</b>
              </div>
              <div class="kpi">
                <span {...help("i/o share")}>i/o share</span>
                <b class="hot">{k().ioShare.toFixed(1)}%</b>
              </div>
              <div class="kpi">
                <span {...help("processes")}>processes</span>
                <b>{k().procs}</b>
              </div>
              <Show when={fileCounts()}>
                {(f) => (
                  <>
                    <div class="kpi">
                      <span {...help("files")}>files</span>
                      <b>{f().total.toLocaleString()}</b>
                    </div>
                    <div class="kpi">
                      <span {...help("i/o files")}>i/o files</span>
                      <b>{f().io.toLocaleString()}</b>
                    </div>
                  </>
                )}
              </Show>
            </div>
          )}
        </Show>

        <Show when={showHelp()}>
          <div class="modal-backdrop" onClick={() => setShowHelp(false)}>
            <div class="modal" onClick={(e) => e.stopPropagation()}>
              <div class="modal-head">
                <b>Keyboard & mouse</b>
                <button class="ghost sm" onClick={() => setShowHelp(false)}>
                  x
                </button>
              </div>
              <table class="help">
                <tbody>
                  <For
                    each={[
                      ["drag / two-finger swipe", "pan"],
                      ["wheel", "scroll lanes"],
                      ["ctrl+wheel / pinch", "zoom at cursor"],
                      ["W / S", "zoom in / out"],
                      ["A / D", "pan left / right"],
                      ["double-click", "zoom in"],
                      ["drag on ruler / shift+drag", "measure a range (stats)"],
                      ["click a slice", "details"],
                      ["Ctrl+F", "find events"],
                      ["Enter / Shift+Enter", "next / previous match"],
                      ["minimap: click / drag", "jump / zoom to region"],
                      ["?", "this help"],
                    ]}
                  >
                    {([k, v]) => (
                      <tr>
                        <td class="help-k">{k}</td>
                        <td class="help-v">{v}</td>
                      </tr>
                    )}
                  </For>
                </tbody>
              </table>
            </div>
          </div>
        </Show>

        <Show when={showSettings()}>
          <div class="modal-backdrop" onClick={() => setShowSettings(false)}>
            <div class="modal" onClick={(e) => e.stopPropagation()}>
              <div class="modal-head">
                <b>Settings</b>
                <button class="ghost sm" onClick={() => setShowSettings(false)}>
                  x
                </button>
              </div>
              <div class="settings-body">
                <div class="settings-row">
                  <span>Theme</span>
                  <button
                    class="ghost sm"
                    onClick={() => applyTheme(theme() === "dark" ? "light" : "dark", true)}
                  >
                    {theme() === "dark" ? "Switch to light" : "Switch to dark"}
                  </button>
                </div>
                <Show when={CONFIG.vscode}>
                  <div class="settings-section">dftracer_server</div>
                  <div class="settings-row">
                    <span>Server path</span>
                    <input
                      type="text"
                      class="settings-input"
                      placeholder="dftracer_server"
                      value={serverPath()}
                      onChange={(e) => {
                        setServerPath(e.currentTarget.value);
                        post({ type: "setServerPath", path: e.currentTarget.value });
                      }}
                    />
                  </div>
                  <div class="settings-section">Load traces</div>
                  <div class="settings-row">
                    <button
                      class="ghost sm"
                      onClick={() => post({ type: "pickTrace", mode: "file" })}
                    >
                      Open trace file...
                    </button>
                    <button
                      class="ghost sm"
                      onClick={() => post({ type: "pickTrace", mode: "dir" })}
                    >
                      Open trace directory...
                    </button>
                  </div>
                </Show>
              </div>
            </div>
          </div>
        </Show>

        <Show when={NEEDS_LOAD}>
          <div class="load-screen">
            <div class="load-card">
              <div class="load-title">DFTracer Trace Viewer</div>
              <Show when={CONFIG.error}>
                <div class="load-error">{CONFIG.error}</div>
              </Show>
              <div class="load-field">
                <label>dftracer_server path (this machine)</label>
                <input
                  type="text"
                  class="settings-input"
                  placeholder="dftracer_server"
                  value={serverPath()}
                  onInput={(e) => setServerPath(e.currentTarget.value)}
                  onChange={(e) => post({ type: "setServerPath", path: e.currentTarget.value })}
                />
                <div class="muted sm">
                  Set the full path if it is not on PATH. When connected to a remote host, this is
                  the path on that host.
                </div>
              </div>
              <div class="load-actions">
                <button class="primary" onClick={() => post({ type: "pickTrace", mode: "dir" })}>
                  Open trace directory...
                </button>
                <button class="ghost" onClick={() => post({ type: "pickTrace", mode: "file" })}>
                  Open trace file...
                </button>
                <Show when={CONFIG.error}>
                  <button class="ghost" onClick={() => post({ type: "retry" })}>
                    Retry
                  </button>
                </Show>
              </div>
            </div>
          </div>
        </Show>

        <div class="workspace">
          <Show when={sidebarOpen()}>
            <aside class="sidebar">
              <div class="sidebar-head">
                <b>
                  Analysis ·{" "}
                  <Show when={anScope()} fallback={<span>whole trace</span>}>
                    {(s) => <span class="scope-tag">selection {formatTime(s().t1 - s().t0)}</span>}
                  </Show>
                </b>
                <div class="head-actions">
                  <Show when={anScope()}>
                    <button class="ghost sm" onClick={clearSelection}>
                      analyze whole trace
                    </button>
                  </Show>
                  <button class="ghost sm" onClick={() => setSidebarOpen(false)}>
                    x
                  </button>
                </div>
              </div>
              <div class="tabs">
                <For each={BOTTOM_TABS}>
                  {([id, label, group]) => (
                    <button
                      class="ghost sm"
                      classList={{ active: bottomTab() === id }}
                      onClick={() => selectBottomTab(id, group)}
                    >
                      {label}
                    </button>
                  )}
                </For>
              </div>
              <div class="analyze-body">
                <Show when={bottomTab() === "eventlog"}>
                  <Show when={eventLogLoading() && !eventLog()}>
                    <div class="muted">loading events...</div>
                  </Show>
                  <Show when={eventLog()}>
                    <table class="kv stats analyze-table op-table evlog-table">
                      <thead>
                        <tr>
                          <th class="num">time</th>
                          <th class="num">dur</th>
                          <th>operation</th>
                          <th>layer</th>
                          <th class="num">pid/tid</th>
                        </tr>
                      </thead>
                      <tbody>
                        <For each={eventLog()}>
                          {(e) => {
                            const name = String(e.name ?? "");
                            return (
                              <tr>
                                <td class="num">{formatTime(Number(e.ts))}</td>
                                <td class="num">{formatTime(Number(e.dur))}</td>
                                <td class="op-name" style={{ color: colorFor(name) }} title={name}>
                                  {name}
                                </td>
                                <td class="op-layer">{String(e.cat || layerOf(name))}</td>
                                <td class="num">
                                  {String(e.pid)}/{String(e.tid)}
                                </td>
                              </tr>
                            );
                          }}
                        </For>
                      </tbody>
                    </table>
                    <Show when={eventLogTruncated()}>
                      <div class="muted">showing first 1000 events (truncated)</div>
                    </Show>
                  </Show>
                </Show>

                <Show when={bottomTab() === "calltree"}>
                  <Show when={anFlameLoading() && !anFlameTree()}>
                    <div class="muted">building call tree...</div>
                  </Show>
                  <div class="bottom-flame">
                    <canvas
                      ref={(el) => {
                        bottomFlame?.destroy();
                        bottomFlame = new Flamegraph(el, {
                          onHover: (node, pct, x, y) =>
                            setBottomFlameHover(node ? { node, pct, x, y } : null),
                        });
                        bottomFlame.setTheme(theme());
                        bottomFlame.setTree(anFlameTree());
                      }}
                    />
                    <Show when={bottomFlameHover()}>{(h) => <FlameTooltip hover={h()} />}</Show>
                  </div>
                </Show>

                <Show when={isFlameTab()}>
                  <Show when={anFlameLoading() && !anFlameTree()}>
                    <div class="muted">building call tree...</div>
                  </Show>
                  <Show when={anFlameTree()}>
                    {(_flame) => {
                      const metricOf = (n: { self: number; total: number }) =>
                        bottomTab() === "bottomup" ? n.self : n.total;
                      const grand = () =>
                        Math.max(
                          1,
                          bottomFns().reduce((s2, n) => s2 + metricOf(n), 0),
                        );
                      return (
                        <table class="kv stats analyze-table op-table">
                          <thead>
                            <tr>
                              <th {...help("operation")}>operation</th>
                              <th {...help("layer")}>layer</th>
                              <th class="num" {...help("self")}>
                                self
                              </th>
                              <th class="num" {...help("total")}>
                                total
                              </th>
                              <th class="bar-col" {...help("wall share")}>
                                {bottomTab() === "bottomup" ? "self share" : "wall share"}
                              </th>
                            </tr>
                          </thead>
                          <tbody>
                            <For each={bottomFns()}>
                              {(n) => {
                                const pct = (metricOf(n) / grand()) * 100;
                                const col = colorFor(n.name);
                                return (
                                  <tr
                                    classList={{ "row-click": true, active: distKey() === n.name }}
                                    onClick={() => loadDist(n.name)}
                                  >
                                    <td
                                      class="analyze-name op-name"
                                      title={n.name}
                                      style={{ color: col }}
                                    >
                                      {n.name}
                                    </td>
                                    <td class="op-layer">{layerOf(n.name)}</td>
                                    <td class="num">{formatTime(n.self)}</td>
                                    <td class="num">{formatTime(n.total)}</td>
                                    <td class="bar-col op-share">
                                      <span
                                        class="bar"
                                        style={{ width: `${pct}%`, background: col }}
                                      />
                                      <span class="op-pct">{pct.toFixed(0)}%</span>
                                    </td>
                                  </tr>
                                );
                              }}
                            </For>
                          </tbody>
                        </table>
                      );
                    }}
                  </Show>
                </Show>

                <Show when={analyzeLoading() && !analyzeStats()}>
                  <div class="muted">aggregating...</div>
                </Show>
                <Show
                  when={
                    analyzeStats() &&
                    bottomTab() !== "eventlog" &&
                    bottomTab() !== "calltree" &&
                    !isFlameTab()
                  }
                >
                  {(_stats) => {
                    const rows = () => analyzeStats()!.names;
                    const grand = () =>
                      Math.max(
                        1,
                        rows().reduce((s2, n) => s2 + n.total, 0),
                      );
                    const isName = () => analyzeTab() === "name" || analyzeTab() === "pid";
                    return (
                      <table class="kv stats analyze-table op-table">
                        <thead>
                          <tr>
                            <th {...help("operation")}>
                              {analyzeTab() === "file"
                                ? "file"
                                : analyzeTab() === "pid"
                                  ? "process"
                                  : "operation"}
                            </th>
                            <th {...help("layer")}>layer</th>
                            <th class="num" {...help("total")}>
                              total
                            </th>
                            <th class="bar-col" {...help("wall share")}>
                              wall share
                            </th>
                          </tr>
                        </thead>
                        <tbody>
                          <For each={rows()}>
                            {(n) => {
                              const label = analyzeLabel(n.name);
                              const clickable = distPredicate(analyzeTab(), n.name) !== null;
                              const pct = (n.total / grand()) * 100;
                              const col = colorFor(n.name);
                              return (
                                <tr
                                  classList={{
                                    "row-click": clickable,
                                    active: distKey() === n.name,
                                  }}
                                  onClick={() => loadDist(n.name)}
                                >
                                  <td
                                    class="analyze-name op-name"
                                    classList={{ path: analyzeTab() === "file" }}
                                    title={label}
                                    style={{ color: isName() ? col : undefined }}
                                  >
                                    <Show when={analyzeTab() === "file"} fallback={label}>
                                      <div class="path-scroll">{label}</div>
                                    </Show>
                                  </td>
                                  <td class="op-layer">
                                    {analyzeTab() === "name" ? layerOf(n.name) : "-"}
                                  </td>
                                  <td class="num">{formatTime(n.total)}</td>
                                  <td class="bar-col op-share">
                                    <span
                                      class="bar"
                                      style={{ width: `${pct}%`, background: col }}
                                    />
                                    <span class="op-pct">{pct.toFixed(0)}%</span>
                                  </td>
                                </tr>
                              );
                            }}
                          </For>
                        </tbody>
                      </table>
                    );
                  }}
                </Show>
              </div>
              <Show when={distKey()}>
                <div class="dist-panel">
                  <div class="dist-head">
                    <span>Duration distribution: {distKey()}</span>
                    <button
                      class="ghost sm"
                      onClick={() => {
                        setDistKey(null);
                        setDistHist(null);
                      }}
                    >
                      x
                    </button>
                  </div>
                  <Show when={distLoading()}>
                    <div class="muted">computing...</div>
                  </Show>
                  <Show when={distHist()}>
                    {(h) => {
                      const barMax = Math.max(1, ...h().buckets.map((b) => b.count));
                      return (
                        <>
                          <div class="dist-stats">
                            <div class="tile">
                              <span>count</span>
                              <b>{h().count.toLocaleString()}</b>
                            </div>
                            <div class="tile">
                              <span>p50</span>
                              <b>{formatTime(h().p50 ?? 0)}</b>
                            </div>
                            <div class="tile">
                              <span>p95</span>
                              <b>{formatTime(h().p95 ?? 0)}</b>
                            </div>
                            <div class="tile">
                              <span>p99</span>
                              <b>{formatTime(h().p99 ?? 0)}</b>
                            </div>
                            <div class="tile">
                              <span>max</span>
                              <b>{formatTime(h().max ?? 0)}</b>
                            </div>
                          </div>
                          <div class="dist-bars">
                            <For each={h().buckets}>
                              {(b) => (
                                <div
                                  class="dist-bar"
                                  style={{ height: `${(b.count / barMax) * 100}%` }}
                                  title={`${formatTime(b.lo)} - ${formatTime(b.hi)}: ${b.count.toLocaleString()}`}
                                />
                              )}
                            </For>
                          </div>
                          <div class="dist-axis">
                            <span>{formatTime(h().min ?? 0)}</span>
                            <span class="muted">duration (log scale)</span>
                            <span>{formatTime(h().max ?? 0)}</span>
                          </div>
                        </>
                      );
                    }}
                  </Show>
                </div>
              </Show>
            </aside>
          </Show>

          <Show when={selected()}>
            <aside class="inspector">
              <div class="sidebar-head">
                <b>SELECTION</b>
                <Show when={selected()}>
                  {(ev) => (
                    <span class="muted sm">
                      {isAggregated(ev())
                        ? `${Number(ev().count).toLocaleString()} EVENTS`
                        : "1 EVENT"}
                    </span>
                  )}
                </Show>
              </div>
              <Show when={selected()}>
                {(ev) => (
                  <div class="insp-scroll">
                    <div class="insp-title">
                      <i style={{ background: colorFor(sliceKey(ev())) }} />
                      {String(ev().name ?? "")}
                      {isAggregated(ev()) ? "" : "()"}
                    </div>
                    <div class="insp-sub">
                      {String(ev().cat ?? "-").toUpperCase()}
                      <Show when={procRank().get(String(ev().pid))}>
                        {(r) => <> · RANK {r()}</>}
                      </Show>{" "}
                      · PROC {String(ev().pid)}
                    </div>
                    <table class="kv insp-kv">
                      <tbody>
                        <For each={inspRows(ev())}>
                          {([k, v]) => (
                            <tr>
                              <td class="k" {...help(k)}>
                                {k}
                              </td>
                              <td class="v">{v}</td>
                            </tr>
                          )}
                        </For>
                      </tbody>
                    </table>
                    <Show when={resolvedRows(ev()).length > 0}>
                      <div class="insp-resolved">
                        <div class="detail-sub">resolved</div>
                        <table class="kv">
                          <tbody>
                            <For each={resolvedRows(ev())}>
                              {([k, v]) => (
                                <tr>
                                  <td class="k">{k}</td>
                                  <td class="v path" title={v}>
                                    {v}
                                  </td>
                                </tr>
                              )}
                            </For>
                          </tbody>
                        </table>
                      </div>
                    </Show>
                    <Show when={isAggregated(ev())}>
                      <div class="muted sm">
                        Merged block. Zoom in to resolve individual events.
                      </div>
                    </Show>
                    <Show when={inspHist()}>
                      {(h) => {
                        const bm = Math.max(1, ...h().buckets.map((b) => b.count));
                        return (
                          <div class="insp-hist">
                            <div class="detail-sub">duration · {String(ev().name)}</div>
                            <div class="dist-bars sm">
                              <For each={h().buckets}>
                                {(b) => (
                                  <div
                                    class="dist-bar"
                                    style={{ height: `${(b.count / bm) * 100}%` }}
                                    title={`${formatTime(b.lo)} - ${formatTime(b.hi)}: ${b.count}`}
                                  />
                                )}
                              </For>
                            </div>
                            <div class="dist-axis">
                              <span>{formatTime(h().min ?? 0)}</span>
                              <span>p50 {formatTime(h().p50 ?? 0)}</span>
                              <span>{formatTime(h().max ?? 0)}</span>
                            </div>
                          </div>
                        );
                      }}
                    </Show>
                    <Show when={inspCallersRoot() && inspCallersRoot()!.children.length > 0}>
                      <div class="insp-sw">
                        <div class="detail-sub">callers</div>
                        <div class="insp-sw-flame">
                          <canvas
                            ref={(el) => {
                              inspCallersFg?.destroy();
                              inspCallersFg = new Flamegraph(el, {
                                onHover: (node, pct, x, y) =>
                                  setInspSwHover(node ? { node, pct, x, y } : null),
                              });
                              inspCallersFg.setTheme(theme());
                              inspCallersFg.setTree(inspCallersRoot());
                            }}
                          />
                        </div>
                      </div>
                    </Show>
                    <Show when={inspCalleesRoot() && inspCalleesRoot()!.children.length > 0}>
                      <div class="insp-sw">
                        <div class="detail-sub">callees</div>
                        <div class="insp-sw-flame">
                          <canvas
                            ref={(el) => {
                              inspCalleesFg?.destroy();
                              inspCalleesFg = new Flamegraph(el, {
                                onHover: (node, pct, x, y) =>
                                  setInspSwHover(node ? { node, pct, x, y } : null),
                              });
                              inspCalleesFg.setTheme(theme());
                              inspCalleesFg.setTree(inspCalleesRoot());
                            }}
                          />
                        </div>
                      </div>
                    </Show>
                    <Show when={inspSwHover()}>{(h) => <FlameTooltip hover={h()} />}</Show>
                    <Show when={flattenArgs(ev()).length > 0}>
                      <div class="insp-args">
                        <div class="detail-sub">args</div>
                        <table class="kv">
                          <tbody>
                            <For each={flattenArgs(ev())}>
                              {([k, v]) => (
                                <tr>
                                  <td class="k">{k}</td>
                                  <td class="v">{v}</td>
                                </tr>
                              )}
                            </For>
                          </tbody>
                        </table>
                      </div>
                    </Show>
                  </div>
                )}
              </Show>
            </aside>
          </Show>

          <div class="views">
            <div class="body" style={{ display: view() === "timeline" ? "flex" : "none" }}>
              <div class="minimap-wrap">
                <canvas ref={minimap} />
              </div>
              <div class="counter-wrap">
                <canvas ref={counterCanvas} />
              </div>
              <div class="canvas-wrap">
                <canvas ref={canvas} />

                <Show
                  when={(() => {
                    const m = meta() as (VizMetadata & { density_count?: number }) | null;
                    return !loading() && m && m.count === 0 && (m.density_count ?? 0) === 0;
                  })()}
                >
                  <div class="empty-state">
                    No events {SINGLE_FILE ? "in this file" : "in this view"}
                  </div>
                </Show>

                <Show when={showLegend() && cats().length > 0}>
                  <div class="legend-panel">
                    <div class="panel-head">
                      <span>Categories ({cats().length})</span>
                      <button class="ghost sm" onClick={() => setShowLegend(false)}>
                        x
                      </button>
                    </div>
                    <div class="legend-list">
                      <For each={cats()}>
                        {(c) => (
                          <button
                            class="legend-item"
                            title={`filter to ${c.name}`}
                            onClick={() => filterByName(c.name)}
                          >
                            <i style={{ background: colorFor(c.name) }} />
                            <span class="legend-name">{c.name}</span>
                            <span class="legend-count">{c.count.toLocaleString()}</span>
                          </button>
                        )}
                      </For>
                    </div>
                  </div>
                </Show>

                <Show when={showGaps() && gaps().length > 0}>
                  <div class="legend-panel gaps-panel">
                    <div class="panel-head">
                      <span>Largest idle periods</span>
                      <button
                        class="ghost sm"
                        onClick={() => {
                          setShowGaps(false);
                          timeline?.setShowGaps(false);
                        }}
                      >
                        x
                      </button>
                    </div>
                    <div class="legend-list">
                      <For each={gaps()}>
                        {(g) => (
                          <button
                            class="legend-item"
                            title="zoom to gap"
                            onClick={() => timeline?.focusRange(g.t0, g.t1)}
                          >
                            <span class="legend-name">{g.label}</span>
                            <span class="legend-count">{formatTime(g.dur)}</span>
                          </button>
                        )}
                      </For>
                    </div>
                  </div>
                </Show>

                <Show when={hover()}>
                  {(h) => (
                    <div
                      class="tooltip"
                      style={{ left: `${h().x + 14}px`, top: `${h().y + 14}px` }}
                    >
                      <div class="tt-name">
                        <i style={{ background: colorFor(sliceKey(h().ev)) }} />
                        {String(h().ev.name ?? "")}
                        <Show when={isAggregated(h().ev)}>
                          <span class="agg-tag">merged</span>
                        </Show>
                      </div>
                      <For each={inspRows(h().ev)}>
                        {([k, v]) => (
                          <div class="tt-row">
                            <span>{k}</span>
                            <b>{v}</b>
                          </div>
                        )}
                      </For>
                      <For each={resolvedRows(h().ev)}>
                        {([k, v]) => (
                          <div class="tt-row">
                            <span>{k}</span>
                            <b>{v}</b>
                          </div>
                        )}
                      </For>
                      <Show when={isAggregated(h().ev)}>
                        <div class="tt-row">
                          <span class="hint2">zoom in to resolve</span>
                        </div>
                      </Show>
                      <For each={flattenArgs(h().ev).slice(0, 6)}>
                        {([k, v]) => (
                          <div class="tt-row">
                            <span>{k}</span>
                            <b>{v}</b>
                          </div>
                        )}
                      </For>
                    </div>
                  )}
                </Show>
              </div>
            </div>

            <div
              class="body flame-body"
              style={{ display: view() === "flamegraph" ? "flex" : "none" }}
            >
              <div class="flame-wrap">
                <canvas ref={flameCanvas} />
                <Show when={flameLoading()}>
                  <div class="flame-overlay muted">building call tree...</div>
                </Show>
                <Show when={flameHover()}>{(h) => <FlameTooltip hover={h()} />}</Show>
              </div>
            </div>

            <Show when={view() === "sandwich"}>
              <div class="body">
                <Show
                  when={flameTree()}
                  fallback={<div class="muted flame-overlay">building call tree...</div>}
                >
                  {(t) => <SandwichView tree={t()} grouped={flameTreeGrouped()} mode={theme()} />}
                </Show>
              </div>
            </Show>

            <Show when={view() === "api"}>
              <div class="body api-body">
                <ApiExplorer />
              </div>
            </Show>
          </div>
        </div>

        <footer class="status">
          <Show when={loading()}>
            <span class="spin">loading...</span>
          </Show>
          <Show when={error()}>
            <span class="err">{error()}</span>
          </Show>
          <Show when={meta()}>
            {(m) => (
              <>
                <span>{m().count.toLocaleString()} events</span>
                <span>summary {summary()}</span>
                <Show when={m().truncated}>
                  <span class="warn">truncated at {m().limit.toLocaleString()}</span>
                </Show>
              </>
            )}
          </Show>
          <Show when={info()}>{(i) => <span>{i().file_count} files</span>}</Show>
          <span class="hint">
            <Show
              when={view() === "flamegraph"}
              fallback="drag or two-finger swipe to pan · pinch or ctrl+wheel to zoom · W/A/S/D keys · drag ruler to measure"
            >
              click a frame to zoom in · click a parent frame to zoom out · width = total time
            </Show>
          </span>
        </footer>
      </div>
    </div>
  );
}

function renderValue(v: unknown): string {
  if (v === null) return "null";
  if (typeof v === "object") return JSON.stringify(v);
  return String(v);
}

function isAggregated(ev: TraceEvent): boolean {
  return (ev as Record<string, unknown>).aggregated === true;
}

// Flatten nested args objects into dotted-key rows (e.g. hw.gpu.mem -> value).
// Arrays and leaves are rendered as values; nested objects recurse.
function flattenObject(obj: Record<string, unknown>, prefix = ""): [string, string][] {
  const out: [string, string][] = [];
  for (const [k, v] of Object.entries(obj)) {
    const key = prefix ? `${prefix}.${k}` : k;
    if (v !== null && typeof v === "object" && !Array.isArray(v)) {
      const rows = flattenObject(v as Record<string, unknown>, key);
      if (rows.length > 0) out.push(...rows);
      else out.push([key, "{}"]);
    } else {
      out.push([key, renderValue(v)]);
    }
  }
  return out;
}

function flattenArgs(ev: TraceEvent): [string, string][] {
  const args = ev.args;
  if (!args || typeof args !== "object") return [];
  return flattenObject(args as Record<string, unknown>);
}
