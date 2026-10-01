// Provenance tab: the lineage graph of a dftracer provenance-mode trace.
//
// Data comes from GET /api/prov/graph (trace::provenance::extract_provenance_graph
// on the server). Entities are ellipses colored by entity type, activities are
// boxes, edges run cause -> activity -> effect. The entity picker selects ids of
// one type and highlights their full lineage (upstream) and impact (downstream).
// Files touched by activities are nodes too (label: path relative to its mount);
// mount points are storage nodes. Edges: read file->activity, write
// activity->file, entity -stored in-> file, file -on-> storage.
import cytoscape from "cytoscape";
import dagre from "cytoscape-dagre";
import {
  createEffect,
  createMemo,
  createSignal,
  For,
  on,
  onCleanup,
  onMount,
  Show,
} from "solid-js";
import { fetchProvGraph, type ProvGraphResponse } from "../data/api";
import "./prov.css";

cytoscape.use(dagre);

// Categorical palette readable on both the dark and light themes.
const PALETTE = [
  "#4e9fd6",
  "#e6a13c",
  "#5fb87a",
  "#d9675f",
  "#a07bd1",
  "#3fb5a8",
  "#d4c04a",
  "#e07fb3",
  "#7f9cc2",
  "#b8865a",
  "#8fbf4d",
  "#c26d9e",
  "#5c8f8a",
  "#d98f4e",
  "#6f7fd6",
  "#9aa55a",
];

// I/O edge kinds; read/write/delete shown by default, open/stat-only optional.
const IO_OPS = [
  ["read", "read", "#3fb5a8"],
  ["write", "write", "#e6a13c"],
  ["delete", "delete / rename", "#d9675f"],
  ["open", "open only", "#8a927c"],
  ["meta", "stat / metadata", "#8a927c"],
] as const;
// Activity<->file link modes. "any" (default) is one link per pair; the others
// are opt-in and their edges are only built when selected.
const IO_MODES = [
  ["any", "any I/O"],
  ["data", "data (read or write)"],
  ["meta", "metadata (open, stat, delete)"],
  ["separate", "read / write separately"],
] as const;
type IoMode = (typeof IO_MODES)[number][0];
const DATA_OPS = new Set(["read", "write"]);
const FILE_COLOR = "#7f9cc2";
const MOUNT_COLOR = "#a07bd1";

const fileId = (path: string) => "F:" + path;
const OP_COLOR = new Map<string, string>(IO_OPS.map(([op, , c]) => [op, c]));
const DIR_COLOR: Record<string, string> = {
  read: OP_COLOR.get("read") ?? "#3fb5a8",
  write: OP_COLOR.get("write") ?? "#e6a13c",
  meta: "#8a927c",
  any: "#9aa7b4",
};

// Activity<->file edges for ONE mode. Only the active mode's edges exist, so
// load and layout stay cheap; switching modes swaps them. "any" is one
// undirected link per pair. Elsewhere direction follows the data: read-like
// (read, open, stat) file -> activity; write and delete/rename activity -> file.
function ioEdges(d: ProvGraphResponse, mode: IoMode): cytoscape.ElementDefinition[] {
  const out: cytoscape.ElementDefinition[] = [];
  const acc = d.accesses ?? [];
  if (mode === "separate") {
    for (const x of acc) {
      const a = "A:" + x.aid;
      const f = fileId(x.path);
      const [source, target] = x.op === "write" || x.op === "delete" ? [a, f] : [f, a];
      out.push({
        data: {
          source,
          target,
          kind: "io",
          op: x.op,
          calls: x.calls,
          bytes: x.bytes,
          color: OP_COLOR.get(x.op) ?? "#8a927c",
        },
      });
    }
    return out;
  }
  type Pair = { a: string; f: string; data: Set<string>; meta: Set<string> };
  const pairs = new Map<string, Pair>();
  for (const x of acc) {
    const a = "A:" + x.aid;
    const f = fileId(x.path);
    const k = a + "\u001f" + f;
    let pr = pairs.get(k);
    if (!pr) {
      pr = { a, f, data: new Set(), meta: new Set() };
      pairs.set(k, pr);
    }
    (DATA_OPS.has(x.op) ? pr.data : pr.meta).add(x.op);
  }
  const push = (pr: Pair, dir: "read" | "write" | "meta" | "any", ops: Set<string>) => {
    const deleteOnly = dir === "meta" && [...ops].every((o) => o === "delete");
    const fromFile = dir === "read" || (dir === "meta" && !deleteOnly);
    const [source, target] = fromFile ? [pr.f, pr.a] : [pr.a, pr.f];
    out.push({ data: { source, target, kind: "ioagg", dir, color: DIR_COLOR[dir] } });
  };
  for (const pr of pairs.values()) {
    if (mode === "any") push(pr, "any", new Set([...pr.data, ...pr.meta]));
    else if (mode === "data") {
      if (pr.data.has("read")) push(pr, "read", pr.data);
      if (pr.data.has("write")) push(pr, "write", pr.data);
    } else if (pr.meta.size) push(pr, "meta", pr.meta);
  }
  return out;
}

// Display name for an entity. With the dftracer entity API the id is a 64-bit
// hash (the app key is not stored), so prefer the uri's file name.
function entityLabel(e: { id: string; uri: string }): string {
  if (e.uri) {
    const parts = e.uri.replace(/\/+$/, "").split("/");
    return parts[parts.length - 1] || e.uri;
  }
  return /^[0-9a-f]{16}$/.test(e.id) ? e.id.slice(0, 10) + "…" : e.id;
}

function fmtBytes(n: number): string {
  if (!n) return "0 B";
  const u = ["B", "KiB", "MiB", "GiB", "TiB"];
  const i = Math.min(u.length - 1, Math.floor(Math.log(n) / Math.log(1024)));
  return `${(n / 1024 ** i).toFixed(i ? 1 : 0)} ${u[i]}`;
}

type Selection = {
  up: number;
  down: number;
  roots: { type: string; id: string }[];
  upTypes: [string, number][];
  downTypes: [string, number][];
  picked: number;
  files: number;
  storage: string[];
};

function count(nodes: cytoscape.NodeCollection): [string, number][] {
  const m = new Map<string, number>();
  nodes.forEach((n) => {
    m.set(n.data("etype"), (m.get(n.data("etype")) ?? 0) + 1);
  });
  return [...m.entries()].sort((a, b) => b[1] - a[1]);
}

export function ProvenanceView(props: { mode: "dark" | "light" }) {
  let host!: HTMLDivElement;
  let cy: cytoscape.Core | undefined;
  const [data, setData] = createSignal<ProvGraphResponse | null>(null);
  const [error, setError] = createSignal<string | null>(null);
  const [hidden, setHidden] = createSignal<Set<string>>(new Set());
  const [ptype, setPtype] = createSignal("");
  const [picked, setPicked] = createSignal<string[]>([]);
  const [detail, setDetail] = createSignal<Record<string, string> | null>(null);
  const [sel, setSel] = createSignal<Selection | null>(null);
  const [showFiles, setShowFiles] = createSignal(true);
  const [showMounts, setShowMounts] = createSignal(true);
  const [ioMode, setIoMode] = createSignal<IoMode>("any");
  const [ops, setOps] = createSignal<Set<string>>(new Set(["read", "write", "delete"]));
  // View scope: when non-empty, only these entities and their neighborhood are drawn.
  const [viewIds, setViewIds] = createSignal<string[]>([]);
  const [viewDraft, setViewDraft] = createSignal<string[]>([]);
  const [viewMode, setViewMode] = createSignal<"direct" | "lineage">("direct");
  const entitiesByType = createMemo(() => {
    const d = data();
    if (!d) return [] as { type: string; items: { hash: string; id: string }[] }[];
    const m = new Map<string, { hash: string; id: string }[]>();
    for (const e of d.entities) {
      if (!m.has(e.type)) m.set(e.type, []);
      m.get(e.type)!.push({ hash: e.hash, id: entityLabel(e) });
    }
    return [...m.entries()]
      .sort((a, b) => a[0].localeCompare(b[0]))
      .map(([type, items]) => ({ type, items: items.sort((a, b) => a.id.localeCompare(b.id)) }));
  });
  const ctrl = new AbortController();
  onCleanup(() => {
    ctrl.abort();
    cy?.destroy();
  });

  // Entity types with the producer's role/description (prov_type records),
  // ordered inputs -> intermediates -> outputs.
  const ROLE_ORDER: Record<string, number> = { input: 0, intermediate: 1, output: 2 };
  const typeInfo = createMemo(
    () => new Map((data()?.types ?? []).map((t) => [t.name, t] as const)),
  );
  const types = createMemo(() => {
    const d = data();
    if (!d) return [] as { t: string; n: number; c: string; role: string; desc: string }[];
    const m = new Map<string, number>();
    d.entities.forEach((e) => m.set(e.type, (m.get(e.type) ?? 0) + 1));
    const info = typeInfo();
    return [...m.keys()]
      .sort()
      .map((t, i) => ({
        t,
        n: m.get(t)!,
        c: PALETTE[i % PALETTE.length],
        role: info.get(t)?.role ?? "",
        desc: info.get(t)?.description ?? "",
      }))
      .sort(
        (a, b) => (ROLE_ORDER[a.role] ?? 3) - (ROLE_ORDER[b.role] ?? 3) || a.t.localeCompare(b.t),
      );
  });
  const ROLE_LABEL: Record<string, string> = {
    input: "Inputs",
    intermediate: "Intermediates",
    output: "Outputs",
  };
  const rolesPresent = createMemo(() => [...new Set(types().map((t) => t.role))]);
  const colorOf = createMemo(() => new Map(types().map((x) => [x.t, x.c])));
  const idsOfType = createMemo((): { hash: string; id: string }[] => {
    const d = data();
    if (!d || !ptype()) return [];
    if (ptype() === "(file)")
      return (d.files ?? [])
        .map((f) => ({ hash: "F:" + f.path, id: `${f.rel}  [${f.mount}]` }))
        .sort((a, b) => a.id.localeCompare(b.id));
    if (ptype() === "(storage)")
      return (d.mounts ?? []).map((m) => ({ hash: "M:" + m.path, id: `${m.path} (${m.fstype})` }));
    return d.entities
      .filter((e) => e.type === ptype())
      .map((e) => ({ hash: e.hash, id: entityLabel(e) }))
      .sort((a, b) => a.id.localeCompare(b.id));
  });

  const css = (v: string) => getComputedStyle(document.documentElement).getPropertyValue(v).trim();

  function style(): cytoscape.Stylesheet[] {
    const text = css("--text") || "#d6d3c0";
    const muted = css("--muted") || "#8a927c";
    const panel = css("--elev") || "#12140f";
    const border = css("--border") || "#262920";
    const accent = css("--accent") || "#e6a13c";
    return [
      {
        selector: 'node[kind="entity"]',
        style: {
          shape: "ellipse",
          "background-color": "data(color)",
          width: 20,
          height: 20,
          label: "data(label)",
          "font-size": 8,
          color: text,
          "text-valign": "bottom",
          "text-margin-y": 3,
          "text-max-width": "120px",
          "text-wrap": "ellipsis",
          "font-family": "ui-monospace, Menlo, monospace",
        },
      },
      {
        selector: 'node[kind="activity"]',
        style: {
          shape: "round-rectangle",
          "background-color": panel,
          "border-width": 1,
          "border-color": border,
          label: "data(label)",
          color: text,
          "font-size": 8,
          "text-valign": "center",
          width: "label",
          height: 18,
          "padding-left": "6px",
          "padding-right": "6px",
        } as cytoscape.Css.Node,
      },
      {
        selector: "edge",
        style: {
          width: 1.1,
          "line-color": muted,
          "target-arrow-color": muted,
          "target-arrow-shape": "triangle",
          "arrow-scale": 0.7,
          "curve-style": "bezier",
          opacity: 0.75,
        },
      },
      { selector: 'edge[rel="used"]', style: { "line-style": "dashed" } },
      {
        selector: 'edge[rel="invalidated"]',
        style: { "line-style": "dashed", "line-color": "#d9675f", "target-arrow-color": "#d9675f" },
      },
      {
        selector: 'edge[rel="updated"]',
        style: { "line-color": "#e6a13c", "target-arrow-color": "#e6a13c" },
      },
      {
        selector: 'edge[rel="erel"]',
        style: {
          "line-style": "dotted",
          "line-color": "#a07bd1",
          "target-arrow-color": "#a07bd1",
          "target-arrow-shape": "vee",
          label: "data(erel)",
          "font-size": 7,
          color: muted,
          "text-rotation": "autorotate",
        },
      },
      {
        selector: 'node[kind="file"]',
        style: {
          shape: "rectangle",
          "background-color": FILE_COLOR,
          width: 14,
          height: 18,
          label: "data(label)",
          "font-size": 7,
          color: muted,
          "text-valign": "bottom",
          "text-margin-y": 2,
          "text-max-width": "160px",
          "text-wrap": "ellipsis",
          "font-family": "ui-monospace, Menlo, monospace",
        },
      },
      {
        selector: 'node[kind="mount"]',
        style: {
          shape: "barrel",
          "background-color": MOUNT_COLOR,
          width: 46,
          height: 34,
          label: "data(label)",
          "font-size": 9,
          "font-weight": "bold",
          color: text,
          "text-valign": "bottom",
          "text-margin-y": 3,
          "text-wrap": "wrap",
          "text-max-width": "180px",
        },
      },
      {
        selector: 'edge[kind="io"]',
        style: { "line-color": "data(color)", "target-arrow-color": "data(color)", width: 1.3 },
      },
      { selector: 'edge[op="open"], edge[op="meta"]', style: { "line-style": "dotted" } },
      {
        selector: 'edge[kind="ioagg"]',
        style: { "line-color": "data(color)", "target-arrow-color": "data(color)", width: 1.3 },
      },
      { selector: 'edge[dir="meta"]', style: { "line-style": "dotted" } },
      { selector: 'edge[dir="any"]', style: { "target-arrow-shape": "none" } },
      { selector: 'edge[op="delete"]', style: { "line-style": "dashed" } },
      {
        selector: 'edge[rel="stored_in"]',
        style: {
          "line-style": "dotted",
          "line-color": FILE_COLOR,
          "target-arrow-shape": "none",
          width: 1,
        },
      },
      {
        selector: 'edge[rel="on"]',
        style: {
          "line-color": MOUNT_COLOR,
          "target-arrow-shape": "none",
          width: 0.8,
          opacity: 0.5,
        },
      },
      { selector: "node[?unreg]", style: { "background-color": css("--danger") || "#d1524e" } },
      { selector: ".gone", style: { display: "none" } },
      { selector: ".faded", style: { opacity: 0.08 } },
      { selector: "node.up", style: { "border-width": 3, "border-color": "#4e9fd6", opacity: 1 } },
      {
        selector: "node.down",
        style: { "border-width": 3, "border-color": "#d9675f", opacity: 1 },
      },
      {
        selector: "edge.up",
        style: { "line-color": "#4e9fd6", "target-arrow-color": "#4e9fd6", width: 2, opacity: 1 },
      },
      {
        selector: "edge.down",
        style: { "line-color": "#d9675f", "target-arrow-color": "#d9675f", width: 2, opacity: 1 },
      },
      {
        selector: "node.pick",
        style: {
          "border-width": 5,
          "border-color": accent,
          width: 30,
          height: 30,
          opacity: 1,
          "z-index": 10,
        },
      },
    ];
  }

  function layout(eles?: cytoscape.Collection) {
    if (!cy) return;
    const target = eles ?? cy.elements(":visible");
    target
      .layout({
        name: "dagre",
        rankDir: "LR",
        nodeSep: 14,
        rankSep: 70,
        animate: false,
      } as cytoscape.LayoutOptions)
      .run();
    cy.fit(target, 30);
  }

  function build(d: ProvGraphResponse) {
    const col = colorOf();
    const els: cytoscape.ElementDefinition[] = [];
    const known = new Set<string>();
    for (const e of d.entities) {
      known.add(e.hash);
      els.push({
        data: {
          id: e.hash,
          kind: "entity",
          etype: e.type,
          label: e.type,
          eid: entityLabel(e),
          hex: e.id,
          store: e.store,
          uri: e.uri,
          color: col.get(e.type),
        },
      });
    }
    const addUnknown = (h: string) => {
      if (known.has(h)) return;
      known.add(h);
      els.push({
        data: {
          id: h,
          kind: "entity",
          etype: "(unregistered)",
          label: "unregistered",
          eid: h,
          store: "?",
          uri: "",
          unreg: true,
        },
      });
    };
    for (const a of d.activities) {
      const id = "A:" + a.aid;
      els.push({
        data: {
          id,
          kind: "activity",
          label: a.name,
          atype: a.activity,
          pid: a.pid,
          dur: a.dur,
          nused: a.used.length,
          ngen: a.generated.length,
        },
      });
      a.used.forEach((h) => {
        addUnknown(h);
        els.push({ data: { source: h, target: id, rel: "used" } });
      });
      a.generated.forEach((h) => {
        addUnknown(h);
        els.push({ data: { source: id, target: h, rel: "generated" } });
      });
      (a.invalidated ?? []).forEach((h) => {
        addUnknown(h);
        els.push({ data: { source: id, target: h, rel: "invalidated" } });
      });
      (a.updated ?? []).forEach((h) => {
        addUnknown(h);
        els.push({ data: { source: id, target: h, rel: "updated" } });
      });
    }
    // Files and storage. File identity is the absolute path.
    for (const m of d.mounts ?? [])
      els.push({
        data: {
          id: "M:" + m.path,
          kind: "mount",
          label: `${m.path}\n${m.fstype}`,
          mpath: m.path,
          fstype: m.fstype,
        },
      });
    for (const f of d.files ?? []) {
      els.push({
        data: {
          id: fileId(f.path),
          kind: "file",
          label: f.rel,
          rel: f.rel,
          mount: f.mount,
          path: f.path,
          fhash: f.fhash,
        },
      });
      els.push({ data: { source: fileId(f.path), target: "M:" + f.mount, rel: "on" } });
    }
    els.push(...ioEdges(d, ioMode()));
    // entity -> entity relations (ER records): contains, derived_from, ...
    for (const r of d.entity_relations ?? []) {
      addUnknown(r.subject);
      addUnknown(r.object);
      els.push({ data: { source: r.subject, target: r.object, rel: "erel", erel: r.relation } });
    }
    for (const ef of d.entity_files ?? [])
      els.push({ data: { source: ef.entity, target: fileId(ef.path), rel: "stored_in" } });
    cy = cytoscape({ container: host, elements: els, style: style(), wheelSensitivity: 0.25 });
    cy.on("tap", "node", (ev) => showDetail(ev.target));
    cy.on("tap", (ev) => {
      if (ev.target === cy) setDetail(null);
    });
    // No layout here: the caller applies the default filters first and lays
    // out only what is visible (laying out every I/O edge level is slow).
  }

  function showDetail(n: cytoscape.NodeSingular) {
    const d = n.data();
    if (d.kind === "entity") {
      const ti = typeInfo().get(d.etype);
      setDetail({
        type: d.etype,
        role: ti?.role || "—",
        "type description": ti?.description || "—",
        id: d.eid,
        store: d.store,
        uri: d.uri || "—",
        hash: d.id,
        relations:
          (n.connectedEdges('edge[rel="erel"]') as cytoscape.EdgeCollection)
            .map((e) =>
              e.source().id() === d.id
                ? `${e.data("erel")} → ${e.target().data("etype")}: ${e.target().data("eid")}`
                : `← ${e.data("erel")} by ${e.source().data("etype")}: ${e.source().data("eid")}`,
            )
            .join("; ") || "—",
        "produced by":
          n
            .incomers("node")
            .map((x) => x.data("label"))
            .join(", ") || "— (root)",
        "used by":
          n
            .outgoers("node")
            .map((x) => x.data("label"))
            .join(", ") || "— (leaf)",
      });
    } else if (d.kind === "file") {
      const acts = new Map((data()?.activities ?? []).map((a) => [a.aid, a.name]));
      const rows = (data()?.accesses ?? []).filter((x) => x.path === d.path);
      const io = (want: (op: string) => boolean) =>
        rows
          .filter((x) => want(x.op))
          .map((x) => `${acts.get(x.aid) ?? x.aid} (${x.op}, ${x.calls}×, ${fmtBytes(x.bytes)})`)
          .join("; ");
      setDetail({
        file: d.rel,
        storage: d.mount,
        "full path": d.path,
        "read by": io((o) => o === "read") || "—",
        "written by": io((o) => o === "write") || "—",
        "metadata by": io((o) => !DATA_OPS.has(o)) || "—",
        entities:
          (n.incomers('edge[rel="stored_in"]') as cytoscape.EdgeCollection)
            .map((e) => `${e.source().data("etype")}: ${e.source().data("eid")}`)
            .join("; ") || "—",
      });
    } else if (d.kind === "mount") {
      setDetail({
        storage: d.mpath,
        filesystem: d.fstype,
        files: String(n.incomers("node").length),
      });
    } else {
      const mine = (data()?.accesses ?? []).filter((x) => "A:" + x.aid === d.id);
      const files = (op: string) =>
        new Set(mine.filter((x) => x.op === op).map((x) => x.path)).size;
      const sum = (op: string) => mine.filter((x) => x.op === op).reduce((t, x) => t + x.bytes, 0);
      setDetail({
        activity: d.label,
        type: d.atype,
        pid: String(d.pid),
        duration: `${(d.dur / 1e3).toFixed(2)} ms`,
        used: String(d.nused),
        generated: String(d.ngen),
        "files read": `${files("read")} (${fmtBytes(sum("read"))})`,
        "files written": `${files("write")} (${fmtBytes(sum("write"))})`,
      });
    }
  }

  // Upstream / downstream over provenance edges (used, generated) only, so the
  // answer does not depend on which I/O link mode is displayed.
  function provWalk(start: cytoscape.CollectionReturnValue, up: boolean) {
    const seen = new Set<string>(start.map((n) => n.id()));
    const out: string[] = [];
    let frontier = start.nodes();
    while (frontier.length) {
      const edges = (up ? frontier.incomers("edge") : frontier.outgoers("edge")).filter(
        '[rel="used"], [rel="generated"], [rel="updated"]',
      ) as cytoscape.EdgeCollection;
      const next = (up ? edges.sources() : edges.targets()).filter((n) => !seen.has(n.id()));
      next.forEach((n) => {
        seen.add(n.id());
        out.push(n.id());
      });
      frontier = next;
    }
    const nodes = cy!.collection(out.map((id) => cy!.getElementById(id)));
    const edges = nodes
      .union(start)
      .connectedEdges('[rel="used"], [rel="generated"], [rel="updated"]')
      .filter((e) => seen.has(e.source().id()) && seen.has(e.target().id()));
    return nodes.union(edges);
  }
  // Files the given activities touched and entities stored in, with storage.
  function filesOf(c: cytoscape.CollectionReturnValue) {
    const acts = c.nodes('[kind="activity"]');
    const ents = c.nodes('[kind="entity"]');
    const ioE = acts.connectedEdges('[kind="io"], [kind="ioagg"]');
    const stE = ents.outgoers('edge[rel="stored_in"]');
    const files = ioE.connectedNodes('[kind="file"]').union(stE.targets());
    const onE = files.outgoers('edge[rel="on"]');
    return files.union(ioE).union(stE).union(onE).union(onE.targets());
  }

  function swapIoEdges() {
    const d = data();
    if (!cy || !d) return;
    cy.batch(() => {
      cy!.remove('edge[kind="io"], edge[kind="ioagg"]');
      cy!.add(ioEdges(d, ioMode()));
    });
  }

  // Node ids drawn for the current view selection, or null for "everything".
  function viewScope(): Set<string> | null {
    if (!cy || !viewIds().length) return null;
    const sel = cy.collection(viewIds().map((h) => cy!.getElementById(h)));
    let keep: cytoscape.CollectionReturnValue;
    if (viewMode() === "lineage") {
      keep = sel.union(provWalk(sel, true)).union(provWalk(sel, false));
      keep = keep.union(filesOf(keep));
    } else {
      // selected entities, the activities and files they touch, and those
      // activities' other inputs/outputs
      const near = sel.closedNeighborhood();
      keep = near.union(near.nodes('[kind="activity"]').neighborhood());
    }
    // storage of every file shown
    keep = keep.union(keep.nodes('[kind="file"]').outgoers('node[kind="mount"]'));
    return new Set(keep.nodes().map((n) => n.id()));
  }

  function applyTypeFilter() {
    if (!cy) return;
    const h = hidden();
    const scope = viewScope();
    const out = (n: cytoscape.NodeSingular) => scope !== null && !scope.has(n.id());
    cy.batch(() => {
      cy!.nodes('[kind="entity"]').forEach((n) => {
        n.toggleClass("gone", out(n) || h.has(n.data("etype")));
      });
      // only the active mode's I/O edges exist; per-op toggles in "separate"
      cy!.edges('[kind="io"]').forEach((e) => {
        e.toggleClass("gone", !ops().has(e.data("op")));
      });
      cy!.nodes('[kind="file"]').forEach((n) => {
        const visibleIo = n
          .connectedEdges('[kind="io"], [kind="ioagg"]')
          .filter((e) => !e.hasClass("gone")).length;
        const viaEntity = n
          .incomers('edge[rel="stored_in"]')
          .sources()
          .filter((m) => !m.hasClass("gone")).length;
        n.toggleClass("gone", out(n) || !showFiles() || (!visibleIo && !viaEntity));
      });
      cy!.nodes('[kind="mount"]').forEach((n) => {
        const any = n.incomers("node").filter((m) => !m.hasClass("gone")).length;
        n.toggleClass("gone", out(n) || !showMounts() || !showFiles() || !any);
      });
      cy!.nodes('[kind="activity"]').forEach((n) => {
        const any = n
          .connectedEdges()
          .connectedNodes()
          .filter((m) => m.id() !== n.id() && !m.hasClass("gone")).length;
        n.toggleClass("gone", out(n) || !any);
      });
    });
    layout();
  }

  function clearTrace() {
    cy?.elements().removeClass("up down pick faded");
    setSel(null);
  }

  function trace() {
    if (!cy) return;
    clearTrace();
    setHidden(new Set<string>());
    cy.elements().removeClass("gone");
    applyTypeFilter(); // keep the file / storage / I/O-op toggles
    const p = cy.collection(picked().map((h) => cy!.getElementById(h)));
    const upP = provWalk(p, true);
    const downP = provWalk(p, false);
    const up = upP.union(filesOf(upP));
    const down = downP.union(filesOf(downP.union(p)));
    cy.elements().addClass("faded");
    up.removeClass("faded").addClass("up");
    down.removeClass("faded").addClass("down");
    p.removeClass("faded").addClass("pick");
    layout(up.union(down).union(p));
    const upE = up.nodes('[kind="entity"]');
    const touched = up.union(down).nodes('[kind="file"]');
    setSel({
      picked: p.length,
      up: upE.length,
      down: down.nodes('[kind="entity"]').length,
      roots: upE
        .filter((n) => n.incomers("node").length === 0)
        .map((n) => ({ type: n.data("etype"), id: n.data("eid") })),
      upTypes: count(upE),
      downTypes: count(down.nodes('[kind="entity"]')),
      files: touched.length,
      storage: [
        ...new Set(
          up
            .union(down)
            .union(p)
            .nodes('[kind="file"]')
            .map((f) => f.data("mount") as string),
        ),
      ],
    });
  }

  onMount(async () => {
    try {
      const d = await fetchProvGraph(ctrl.signal);
      setData(d);
      if (d.entities.length || d.activities.length) {
        build(d);
        applyTypeFilter(); // apply the default I/O-op filter (hides open/stat)
      }
    } catch (e) {
      if (!ctrl.signal.aborted) setError(e instanceof Error ? e.message : String(e));
    }
  });
  // Re-read the theme tokens when the app's dark/light mode flips.
  createEffect(
    on(
      () => props.mode,
      () => cy?.style(style()),
      { defer: true },
    ),
  );

  return (
    <div class="prov">
      <aside class="prov-rail">
        <Show when={error()}>
          <div class="prov-error">Could not load the provenance graph: {error()}</div>
        </Show>
        <Show
          when={data()}
          fallback={
            <Show when={!error()}>
              <div class="muted">Scanning trace for provenance records…</div>
            </Show>
          }
        >
          {(d) => (
            <>
              <section>
                <h3>Trace</h3>
                <dl class="prov-stats">
                  <dt>entities</dt>
                  <dd>{d().stats.entities}</dd>
                  <dt>activities</dt>
                  <dd>{d().stats.activities}</dd>
                  <dt>dangling</dt>
                  <dd classList={{ bad: d().stats.dangling_hashes > 0 }}>
                    {d().stats.dangling_hashes}
                  </dd>
                  <dt>isolated</dt>
                  <dd>{d().stats.isolated_entities}</dd>
                  <Show when={d().stats.files !== undefined}>
                    <dt>files</dt>
                    <dd>{d().stats.files}</dd>
                    <dt>storage</dt>
                    <dd>{d().stats.mounts}</dd>
                    <dt>I/O calls in activities</dt>
                    <dd>{d().stats.io_attributed}</dd>
                  </Show>
                  <dt>truncated</dt>
                  <dd classList={{ bad: d().stats.truncated_activities > 0 }}>
                    {d().stats.truncated_activities}
                  </dd>
                </dl>
                <Show when={!d().stats.entities && !d().stats.activities}>
                  <p class="muted">
                    No provenance records in this trace. Record them with dftracer_prov
                    (DFT_PROV_ACTIVITY / prov.activity) and run with DFTRACER_INC_METADATA=1.
                  </p>
                </Show>
              </section>
              <section class="prov-trace prov-view">
                <h3>View</h3>
                <label for="prov-view">Entities to show</label>
                <select
                  id="prov-view"
                  multiple
                  size={10}
                  onChange={(e) =>
                    setViewDraft([...e.currentTarget.selectedOptions].map((o) => o.value))
                  }
                >
                  <For each={entitiesByType()}>
                    {(g) => (
                      <optgroup
                        label={`${typeInfo().get(g.type)?.role ?? "?"} · ${g.type} (${g.items.length})`}
                      >
                        <For each={g.items}>
                          {(it) => (
                            <option value={it.hash} selected={viewDraft().includes(it.hash)}>
                              {it.id}
                            </option>
                          )}
                        </For>
                      </optgroup>
                    )}
                  </For>
                </select>
                <div class="prov-row">
                  <For each={rolesPresent().filter((r) => ROLE_LABEL[r])}>
                    {(role) => (
                      <button
                        type="button"
                        title={`Select every ${role} entity`}
                        onClick={() =>
                          setViewDraft(
                            (data()?.entities ?? [])
                              .filter((e) => typeInfo().get(e.type)?.role === role)
                              .map((e) => e.hash),
                          )
                        }
                      >
                        All {ROLE_LABEL[role].toLowerCase()}
                      </button>
                    )}
                  </For>
                </div>
                <div class="prov-radio" role="radiogroup" aria-label="View scope">
                  <label>
                    <input
                      type="radio"
                      name="prov-view-mode"
                      checked={viewMode() === "direct"}
                      onChange={() => setViewMode("direct")}
                    />
                    selected + direct links
                  </label>
                  <label>
                    <input
                      type="radio"
                      name="prov-view-mode"
                      checked={viewMode() === "lineage"}
                      onChange={() => setViewMode("lineage")}
                    />
                    selected + full lineage &amp; impact
                  </label>
                </div>
                <div class="prov-row">
                  <button
                    type="button"
                    class="primary"
                    disabled={!viewDraft().length}
                    onClick={() => {
                      clearTrace();
                      setViewIds(viewDraft());
                      applyTypeFilter();
                    }}
                  >
                    Show selected ({viewDraft().length})
                  </button>
                  <button
                    type="button"
                    disabled={!viewIds().length}
                    onClick={() => {
                      clearTrace();
                      setViewIds([]);
                      setViewDraft([]);
                      applyTypeFilter();
                    }}
                  >
                    Show all
                  </button>
                </div>
                <Show when={viewIds().length}>
                  <p class="muted">
                    Showing {viewIds().length} selected{" "}
                    {viewMode() === "lineage"
                      ? "with full lineage and impact"
                      : "with direct links"}
                    . Ctrl/⌘-click or Shift-click to pick several.
                  </p>
                </Show>
              </section>
              <section class="prov-trace">
                <h3>Trace entities</h3>
                <label for="prov-type">Entity type</label>
                <select
                  id="prov-type"
                  value={ptype()}
                  onChange={(e) => {
                    setPtype(e.currentTarget.value);
                    setPicked([]);
                  }}
                >
                  <option value="">Choose a type…</option>
                  <Show when={(d().files ?? []).length}>
                    <option value="(file)">(file) ({d().files!.length})</option>
                    <option value="(storage)">(storage) ({(d().mounts ?? []).length})</option>
                  </Show>
                  <For each={types()}>
                    {(t) => (
                      <option value={t.t}>
                        {t.t} ({t.n})
                      </option>
                    )}
                  </For>
                </select>
                <label for="prov-ids">Entity ids</label>
                <select
                  id="prov-ids"
                  multiple
                  size={7}
                  disabled={!ptype()}
                  onChange={(e) =>
                    setPicked([...e.currentTarget.selectedOptions].map((o) => o.value))
                  }
                >
                  <For each={idsOfType()}>
                    {(e) => (
                      <option value={e.hash} selected={picked().includes(e.hash)}>
                        {e.id}
                      </option>
                    )}
                  </For>
                </select>
                <div class="prov-row">
                  <button
                    type="button"
                    disabled={!ptype()}
                    onClick={() => setPicked(idsOfType().map((e) => e.hash))}
                  >
                    Select all
                  </button>
                  <button type="button" class="primary" disabled={!picked().length} onClick={trace}>
                    Trace lineage + impact
                  </button>
                  <button
                    type="button"
                    onClick={() => {
                      clearTrace();
                      setPicked([]);
                      layout();
                    }}
                  >
                    Clear
                  </button>
                </div>
                <div class="prov-keys">
                  <span>
                    <i class="k up" />
                    lineage
                  </span>
                  <span>
                    <i class="k pick" />
                    selected
                  </span>
                  <span>
                    <i class="k down" />
                    impact
                  </span>
                </div>
                <Show when={sel()}>
                  {(s) => (
                    <div class="prov-result">
                      <p>
                        <b>{s().picked}</b> selected · <b>{s().up}</b> upstream · <b>{s().down}</b>{" "}
                        downstream entities · <b>{s().files}</b> files
                      </p>
                      <h4 class="up">Derived from</h4>
                      <ul>
                        <For each={s().upTypes} fallback={<li>nothing (selection is a root)</li>}>
                          {([t, n]) => (
                            <li>
                              {t} × {n}
                            </li>
                          )}
                        </For>
                      </ul>
                      <h4>Root inputs</h4>
                      <ul>
                        <For each={s().roots} fallback={<li>none</li>}>
                          {(r) => (
                            <li>
                              {r.type}: {r.id}
                            </li>
                          )}
                        </For>
                      </ul>
                      <h4>Storage touched</h4>
                      <ul>
                        <For each={s().storage} fallback={<li>none</li>}>
                          {(m) => <li>{m}</li>}
                        </For>
                      </ul>
                      <h4 class="down">Affects</h4>
                      <ul>
                        <For each={s().downTypes} fallback={<li>nothing downstream</li>}>
                          {([t, n]) => (
                            <li>
                              {t} × {n}
                            </li>
                          )}
                        </For>
                      </ul>
                    </div>
                  )}
                </Show>
              </section>
              <Show when={(d().files ?? []).length}>
                <section>
                  <h3>Files and storage</h3>
                  <label class="prov-type">
                    <input
                      type="checkbox"
                      checked={showFiles()}
                      onChange={(e) => {
                        setShowFiles(e.currentTarget.checked);
                        applyTypeFilter();
                      }}
                    />
                    <span class="sw sq" style={{ background: FILE_COLOR }} />
                    <span class="nm">files (path from mount)</span>
                    <span class="ct">{d().files!.length}</span>
                  </label>
                  <label class="prov-type">
                    <input
                      type="checkbox"
                      checked={showMounts()}
                      onChange={(e) => {
                        setShowMounts(e.currentTarget.checked);
                        applyTypeFilter();
                      }}
                    />
                    <span class="sw" style={{ background: MOUNT_COLOR }} />
                    <span class="nm">storage (mount point)</span>
                    <span class="ct">{(d().mounts ?? []).length}</span>
                  </label>
                  <div class="prov-sub">Activity ↔ file links</div>
                  <div class="prov-radio" role="radiogroup" aria-label="Activity to file links">
                    <For each={IO_MODES}>
                      {([m, label]) => (
                        <label>
                          <input
                            type="radio"
                            name="prov-io-mode"
                            checked={ioMode() === m}
                            onChange={() => {
                              setIoMode(m);
                              swapIoEdges();
                              applyTypeFilter();
                            }}
                          />
                          {label}
                        </label>
                      )}
                    </For>
                  </div>
                  <div class="prov-keys">
                    <span>
                      <i class="k" style={{ background: "#3fb5a8" }} />
                      read: file → activity
                    </span>
                    <span>
                      <i class="k" style={{ background: "#e6a13c" }} />
                      write: activity → file
                    </span>
                    <span>
                      <i class="k meta" />
                      metadata: file → activity (dotted)
                    </span>
                  </div>
                  <For each={ioMode() === "separate" ? IO_OPS : []}>
                    {([op, label, c]) => (
                      <label class="prov-type prov-indent">
                        <input
                          type="checkbox"
                          checked={ops().has(op)}
                          onChange={(e) => {
                            const o = new Set(ops());
                            if (e.currentTarget.checked) o.add(op);
                            else o.delete(op);
                            setOps(o);
                            applyTypeFilter();
                          }}
                        />
                        <span class="sw ln" style={{ background: c }} />
                        <span class="nm">{label}</span>
                        <span class="ct">
                          {(d().accesses ?? []).filter((x) => x.op === op).length}
                        </span>
                      </label>
                    )}
                  </For>
                </section>
              </Show>
              <section>
                <h3>Entity types</h3>
                <For each={rolesPresent()}>
                  {(role) => (
                    <>
                      <div class="prov-sub">{ROLE_LABEL[role] ?? "Undescribed"}</div>
                      <For each={types().filter((x) => x.role === role)}>
                        {(t) => (
                          <label class="prov-type" title={t.desc || "no description recorded"}>
                            <input
                              type="checkbox"
                              checked={!hidden().has(t.t)}
                              onChange={(e) => {
                                const h = new Set(hidden());
                                if (e.currentTarget.checked) h.delete(t.t);
                                else h.add(t.t);
                                setHidden(h);
                                applyTypeFilter();
                              }}
                            />
                            <span class="sw" style={{ background: t.c }} />
                            <span class="nm">{t.t}</span>
                            <span class="ct">{t.n}</span>
                          </label>
                        )}
                      </For>
                    </>
                  )}
                </For>
              </section>
            </>
          )}
        </Show>
      </aside>
      <div class="prov-canvas">
        <div ref={host} class="prov-cy" role="img" aria-label="Provenance graph" />
        <Show when={detail()}>
          {(d) => (
            <div class="prov-detail">
              <dl>
                <For each={Object.entries(d())}>
                  {([k, v]) => (
                    <>
                      <dt>{k}</dt>
                      <dd>{v}</dd>
                    </>
                  )}
                </For>
              </dl>
            </div>
          )}
        </Show>
        <div class="prov-hint">
          Click a node for details · ellipse: entity · box: activity · square: file · barrel:
          storage · arrows: generated / used (dashed) / read / write
        </div>
      </div>
    </div>
  );
}
