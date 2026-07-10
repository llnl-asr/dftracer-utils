import { createEffect, createMemo, createSignal, For, onCleanup, onMount, Show } from "solid-js";
import type { FlameNode } from "../data/types";
import { Flamegraph } from "./flamegraph";
import { calleesTree, callersTree, functionList, type FnRow } from "./sandwich";
import { colorFor } from "../timeline/color";
import { formatTime } from "../timeline/format";
import { FlameTooltip, type FlameHover } from "./FlameTooltip";

// Speedscope-style sandwich: a table of functions (self/total across all call
// sites); selecting one shows its callers (inverted) and callees flamegraphs. A
// grouped tree (root children are per-process frames) enables a process scope.
export function SandwichView(props: {
  tree: FlameNode | null;
  grouped: boolean;
  mode: "dark" | "light";
}) {
  let callersCanvas!: HTMLCanvasElement;
  let calleesCanvas!: HTMLCanvasElement;
  let callersFg: Flamegraph | undefined;
  let calleesFg: Flamegraph | undefined;

  const [selected, setSelected] = createSignal<string | null>(null);
  const [sortKey, setSortKey] = createSignal<"self" | "total">("self");
  const [hover, setHover] = createSignal<FlameHover | null>(null);
  const [ready, setReady] = createSignal(false);
  const [proc, setProc] = createSignal("all"); // "all" or a process frame name

  const procNames = createMemo<string[]>(() =>
    props.grouped && props.tree ? props.tree.children.map((c) => c.name) : [],
  );

  // The subtree roots to aggregate over: all process frames, one process, or the
  // single whole-trace root when not grouped.
  const roots = createMemo<FlameNode[]>(() => {
    const t = props.tree;
    if (!t) return [];
    if (!props.grouped) return [t];
    if (proc() === "all") return t.children;
    return t.children.filter((c) => c.name === proc());
  });

  const rows = createMemo<FnRow[]>(() => {
    const list = functionList(roots());
    const k = sortKey();
    list.sort((a, b) => b[k] - a[k]);
    return list;
  });

  // Keep a valid selection (default to the heaviest function).
  createEffect(() => {
    const r = rows();
    const cur = selected();
    if (r.length && (cur === null || !r.some((x) => x.name === cur))) {
      setSelected(r[0].name);
    }
  });

  const onHover = (n: FlameNode | null, pct: number, x: number, y: number) => {
    setHover(n ? { node: n, pct, x, y } : null);
  };

  onMount(() => {
    callersFg = new Flamegraph(callersCanvas, { onHover });
    calleesFg = new Flamegraph(calleesCanvas, { onHover });
    setReady(true);
  });

  createEffect(() => {
    if (!ready()) return;
    callersFg?.setTheme(props.mode);
    calleesFg?.setTheme(props.mode);
  });
  onCleanup(() => {
    callersFg?.destroy();
    calleesFg?.destroy();
  });

  createEffect(() => {
    if (!ready()) return;
    const r = roots();
    const name = selected();
    if (!r.length || !name) {
      callersFg?.setTree(null);
      calleesFg?.setTree(null);
      return;
    }
    callersFg?.setTree(callersTree(r, name));
    calleesFg?.setTree(calleesTree(r, name));
  });

  return (
    <div class="sandwich">
      <div class="sw-table">
        <Show when={procNames().length > 0}>
          <div class="sw-proc">
            <span>process</span>
            <select value={proc()} onChange={(e) => setProc(e.currentTarget.value)}>
              <option value="all">All ({procNames().length})</option>
              <For each={procNames()}>{(n) => <option value={n}>{n}</option>}</For>
            </select>
          </div>
        </Show>
        <div class="sw-thead">
          <button
            class="sw-h num"
            classList={{ active: sortKey() === "self" }}
            onClick={() => setSortKey("self")}
          >
            self
          </button>
          <button
            class="sw-h num"
            classList={{ active: sortKey() === "total" }}
            onClick={() => setSortKey("total")}
          >
            total
          </button>
          <span class="sw-h">function</span>
        </div>
        <div class="sw-rows">
          <For each={rows()}>
            {(r) => (
              <button
                class="sw-row"
                classList={{ active: selected() === r.name }}
                onClick={() => setSelected(r.name)}
              >
                <span class="num">{formatTime(r.self)}</span>
                <span class="num">{formatTime(r.total)}</span>
                <span class="sw-name" title={r.name}>
                  <i style={{ background: colorFor(r.name) }} />
                  {r.name}
                </span>
              </button>
            )}
          </For>
        </div>
      </div>
      <div class="sw-graphs">
        <div class="sw-panel">
          <div class="sw-label">Callers of {selected() ?? "-"}</div>
          <div class="sw-canvas">
            <canvas ref={callersCanvas} />
          </div>
        </div>
        <div class="sw-panel">
          <div class="sw-label">Callees of {selected() ?? "-"}</div>
          <div class="sw-canvas">
            <canvas ref={calleesCanvas} />
          </div>
        </div>
      </div>
      <Show when={hover()}>{(h) => <FlameTooltip hover={h()} />}</Show>
    </div>
  );
}
