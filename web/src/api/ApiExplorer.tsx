import { createResource, createSignal, For, Show } from "solid-js";
import { apiRequest } from "../data/api";

interface OApiParam {
  name: string;
  required?: boolean;
  schema?: { default?: string };
}
interface OApiOp {
  summary?: string;
  tags?: string[];
  parameters?: OApiParam[];
}
interface OpenApi {
  paths: Record<string, { get?: OApiOp }>;
}

async function loadSpec(): Promise<OpenApi> {
  const r = await apiRequest("/api/openapi.json");
  if (r.status !== 200) {
    throw new Error(`server returned ${r.status || "no response"}`);
  }
  try {
    return JSON.parse(r.body) as OpenApi;
  } catch {
    throw new Error("response was not valid JSON");
  }
}

function groupByTag(spec: OpenApi): [string, { path: string; op: OApiOp }[]][] {
  const groups = new Map<string, { path: string; op: OApiOp }[]>();
  for (const [path, item] of Object.entries(spec.paths)) {
    const op = item.get;
    if (!op) continue;
    const tag = op.tags?.[0] ?? "Other";
    if (!groups.has(tag)) groups.set(tag, []);
    groups.get(tag)!.push({ path, op });
  }
  return [...groups.entries()];
}

// Interactive, palette-matched API reference: renders the server's OpenAPI spec
// as endpoints with editable query knobs and a live Send against this server.
export function ApiExplorer() {
  const [spec] = createResource(loadSpec);
  return (
    <div class="api-view">
      <div class="api-view-head">
        <h2>API</h2>
        <a href={apiSpecHref()} target="_blank" rel="noreferrer">
          openapi.json
        </a>
      </div>
      <Show
        when={spec()}
        fallback={
          <Show when={spec.error} fallback={<div class="api-loading">loading spec ...</div>}>
            <div class="api-error">
              Could not load the API spec: {String((spec.error as Error).message)}
            </div>
          </Show>
        }
      >
        <For each={groupByTag(spec()!)}>
          {([tag, eps]) => (
            <>
              <h3 class="api-group">{tag}</h3>
              <For each={eps}>{(ep) => <Endpoint path={ep.path} op={ep.op} />}</For>
            </>
          )}
        </For>
      </Show>
    </div>
  );
}

function apiSpecHref(): string {
  const base = (window as { __DFTRACER__?: { apiBase?: string } }).__DFTRACER__?.apiBase ?? "";
  return (base ? base.replace(/\/$/, "") : "") + "/api/openapi.json";
}

function Endpoint(props: { path: string; op: OApiOp }) {
  const params = () => props.op.parameters ?? [];
  const [vals, setVals] = createSignal<Record<string, string>>(
    Object.fromEntries(params().map((p) => [p.name, p.schema?.default ?? ""])),
  );
  const [resp, setResp] = createSignal<{ status: number; body: string } | null>(null);
  const [busy, setBusy] = createSignal(false);

  // The path + query the Send button will request, from the filled-in knobs.
  const reqPath = () => {
    const qs = new URLSearchParams();
    for (const p of params()) {
      const v = (vals()[p.name] ?? "").trim();
      if (v) qs.set(p.name, v);
    }
    const q = qs.toString();
    return props.path + (q ? "?" + q : "");
  };

  const send = async () => {
    setBusy(true);
    const r = await apiRequest(reqPath());
    let body = r.body;
    try {
      body = JSON.stringify(JSON.parse(r.body), null, 2);
    } catch {
      /* keep raw (NDJSON / html) */
    }
    if (body.length > 6000) body = body.slice(0, 6000) + "\n...";
    setResp({ status: r.status, body });
    setBusy(false);
  };

  return (
    <div class="api-ep">
      <div class="api-ep-head">
        <span class="api-method">GET</span>
        <code class="api-path">{props.path}</code>
        <button class="api-send" disabled={busy()} onClick={send}>
          {busy() ? "..." : "Send"}
        </button>
      </div>
      <Show when={props.op.summary}>
        <div class="api-summary">{props.op.summary}</div>
      </Show>
      <Show when={params().length}>
        <div class="api-params">
          <For each={params()}>
            {(p) => (
              <label class="api-param">
                <span>
                  {p.name}
                  {p.required ? " *" : ""}
                </span>
                <input
                  value={vals()[p.name] ?? ""}
                  placeholder={p.schema?.default ?? ""}
                  onInput={(e) => setVals({ ...vals(), [p.name]: e.currentTarget.value })}
                />
              </label>
            )}
          </For>
        </div>
      </Show>
      <div class="api-req">
        GET <b>{reqPath()}</b>
      </div>
      <Show when={resp()}>
        {(r) => (
          <div class="api-resp">
            <span class="api-status" classList={{ ok: r().status >= 200 && r().status < 300 }}>
              {r().status || "error"}
            </span>
            <pre>{r().body}</pre>
          </div>
        )}
      </Show>
    </div>
  );
}
