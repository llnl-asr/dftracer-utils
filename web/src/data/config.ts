// Runtime config. When served by dftracer_server the page uses relative API
// paths and reads file/token from its URL. When hosted in the VS Code webview
// the extension injects window.__DFTRACER__ with an absolute API base (or "" if
// no server is running yet), plus the file/token and a vscode flag.
export interface DftracerConfig {
  apiBase: string; // "" = same-origin (standalone server)
  file: string; // single-file scope, or ""
  token: string; // access token, or ""
  vscode: boolean; // running inside the VS Code webview
  error: string; // server-start error to show on the load screen, or ""
}

declare global {
  interface Window {
    __DFTRACER__?: Partial<DftracerConfig>;
    acquireVsCodeApi?: () => { postMessage: (m: unknown) => void };
  }
}

function read(): DftracerConfig {
  const inj = typeof window !== "undefined" ? window.__DFTRACER__ : undefined;
  if (inj) {
    return {
      apiBase: inj.apiBase ?? "",
      file: inj.file ?? "",
      token: inj.token ?? "",
      vscode: !!inj.vscode,
      error: inj.error ?? "",
    };
  }
  const q = typeof window !== "undefined" ? new URLSearchParams(window.location.search) : null;
  return {
    apiBase: "",
    file: q?.get("file") ?? "",
    token: q?.get("token") ?? "",
    vscode: false,
    error: "",
  };
}

export const CONFIG = read();
