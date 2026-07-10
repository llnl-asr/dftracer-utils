import { CONFIG } from "./config";

// Messages the webview sends to the extension host.
type OutMsg =
  | { type: "pickTrace"; mode: "file" | "dir" }
  | { type: "setServerPath"; path: string }
  | { type: "getServerPath" }
  | { type: "retry" };

// acquireVsCodeApi may only be called once per session.
let api: { postMessage: (m: unknown) => void } | undefined;
function host() {
  if (!CONFIG.vscode) return undefined;
  if (!api && typeof window !== "undefined" && window.acquireVsCodeApi)
    api = window.acquireVsCodeApi();
  return api;
}

export function post(msg: OutMsg): void {
  host()?.postMessage(msg);
}

// Subscribe to messages from the extension host (e.g. the current server path).
export function onHostMessage(cb: (msg: { type: string; [k: string]: unknown }) => void): void {
  if (typeof window === "undefined") return;
  window.addEventListener("message", (e) => cb(e.data));
}
