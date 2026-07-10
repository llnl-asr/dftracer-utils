import type { FlameNode } from "../data/types";
import { colorFor } from "../timeline/color";
import { vizTheme, type ThemeMode, type VizTheme } from "../timeline/theme";

const ROW_H = 18;
const MIN_PX = 0.6; // skip nodes narrower than this many pixels

interface Rect {
  node: FlameNode;
  x0: number; // pixels
  x1: number;
  y: number;
  ancestor: boolean; // a full-width frame above the focused node
}

export interface FlameCallbacks {
  onHover?: (node: FlameNode | null, pctRoot: number, clientX: number, clientY: number) => void;
}

// Icicle-style flamegraph: the focused node spans the full width, its ancestors
// stack as full-width frames above it, and its subtree fans out below with each
// node's width proportional to its inclusive total. Click a node to zoom.
export class Flamegraph {
  private canvas: HTMLCanvasElement;
  private ctx: CanvasRenderingContext2D;
  private cb: FlameCallbacks;
  private dpr = Math.max(1, window.devicePixelRatio || 1);
  private cssW = 0;
  private cssH = 0;
  private ro?: ResizeObserver;
  private th: VizTheme = vizTheme("dark");

  private root: FlameNode | null = null;
  private focus: FlameNode | null = null;
  private path: FlameNode[] = []; // root .. focus
  private rects: Rect[] = [];
  private hovered: FlameNode | null = null;
  private rootTotal = 1;

  constructor(canvas: HTMLCanvasElement, cb: FlameCallbacks = {}) {
    this.canvas = canvas;
    this.cb = cb;
    const ctx = canvas.getContext("2d");
    if (!ctx) throw new Error("2D canvas context unavailable");
    this.ctx = ctx;
    canvas.addEventListener("mousemove", this.onMove);
    canvas.addEventListener("mouseleave", this.onLeave);
    canvas.addEventListener("click", this.onClick);
    this.ro = new ResizeObserver(() => this.resize());
    this.ro.observe(canvas.parentElement ?? canvas);
    this.resize();
  }

  destroy(): void {
    this.ro?.disconnect();
    this.canvas.removeEventListener("mousemove", this.onMove);
    this.canvas.removeEventListener("mouseleave", this.onLeave);
    this.canvas.removeEventListener("click", this.onClick);
  }

  setTheme(mode: ThemeMode): void {
    this.th = vizTheme(mode);
    this.render();
  }

  setTree(root: FlameNode | null): void {
    this.root = root;
    this.focus = root;
    this.path = root ? [root] : [];
    this.rootTotal = Math.max(1, root?.total ?? 1);
    this.layout();
    this.render();
  }

  private setFocus(node: FlameNode, path: FlameNode[]): void {
    this.focus = node;
    this.path = path;
    this.layout();
    this.render();
  }

  private resize(): void {
    const parent = this.canvas.parentElement;
    const w = parent ? parent.clientWidth : this.canvas.clientWidth;
    const h = parent ? parent.clientHeight : this.canvas.clientHeight;
    this.cssW = w;
    this.cssH = h;
    this.canvas.width = Math.round(w * this.dpr);
    this.canvas.height = Math.round(h * this.dpr);
    this.canvas.style.width = `${w}px`;
    this.canvas.style.height = `${h}px`;
    this.layout();
    this.render();
  }

  private layout(): void {
    this.rects = [];
    if (!this.focus || !this.root) return;
    const baseDepth = this.path.length - 1;
    // Ancestor frames above the focus, each full width.
    for (let d = 0; d < baseDepth; d++) {
      this.rects.push({
        node: this.path[d],
        x0: 0,
        x1: this.cssW,
        y: d * ROW_H,
        ancestor: true,
      });
    }
    const rec = (n: FlameNode, x0: number, x1: number, depth: number): void => {
      this.rects.push({ node: n, x0, x1, y: depth * ROW_H, ancestor: false });
      const w = x1 - x0;
      const tot = n.total || 1;
      const kids = [...n.children].sort((a, b) => b.total - a.total);
      let cx = x0;
      for (const c of kids) {
        const cw = (c.total / tot) * w;
        if (cw >= MIN_PX) rec(c, cx, cx + cw, depth + 1);
        cx += cw;
      }
    };
    rec(this.focus, 0, this.cssW, baseDepth);
  }

  private render(): void {
    const ctx = this.ctx;
    ctx.save();
    ctx.scale(this.dpr, this.dpr);
    ctx.clearRect(0, 0, this.cssW, this.cssH);
    ctx.fillStyle = this.th.plotBg;
    ctx.fillRect(0, 0, this.cssW, this.cssH);
    ctx.font = '11px system-ui, -apple-system, "Segoe UI", sans-serif';
    ctx.textBaseline = "middle";

    for (const r of this.rects) {
      const w = r.x1 - r.x0;
      if (w < MIN_PX) continue;
      const base = colorFor(r.node.name);
      const hot = r.node === this.hovered;
      ctx.fillStyle = r.ancestor ? this.th.flameAncestor : base;
      ctx.globalAlpha = r.ancestor ? 0.7 : 1;
      ctx.fillRect(r.x0, r.y, Math.max(1, w - 1), ROW_H - 1);
      ctx.globalAlpha = 1;
      if (hot) {
        ctx.strokeStyle = this.th.accent;
        ctx.lineWidth = 1;
        ctx.strokeRect(r.x0 + 0.5, r.y + 0.5, Math.max(1, w - 1) - 1, ROW_H - 2);
      }
      if (w > 32) {
        ctx.save();
        ctx.beginPath();
        ctx.rect(r.x0, r.y, w - 2, ROW_H - 1);
        ctx.clip();
        ctx.fillStyle = r.ancestor ? this.th.flameAncestorText : this.th.flameLabelDark;
        ctx.fillText(r.node.name, r.x0 + 4, r.y + ROW_H / 2);
        ctx.restore();
      }
    }
    ctx.restore();
  }

  private hitTest(x: number, y: number): Rect | null {
    for (const r of this.rects) {
      if (x >= r.x0 && x <= r.x1 && y >= r.y && y <= r.y + ROW_H - 1) return r;
    }
    return null;
  }

  private local(e: MouseEvent): { x: number; y: number } {
    const r = this.canvas.getBoundingClientRect();
    return { x: e.clientX - r.left, y: e.clientY - r.top };
  }

  private onMove = (e: MouseEvent): void => {
    const { x, y } = this.local(e);
    const hit = this.hitTest(x, y);
    const node = hit?.node ?? null;
    if (node !== this.hovered) {
      this.hovered = node;
      this.render();
    }
    this.canvas.style.cursor = node ? "pointer" : "default";
    const pct = node ? (node.total / this.rootTotal) * 100 : 0;
    this.cb.onHover?.(node, pct, e.clientX, e.clientY);
  };

  private onLeave = (): void => {
    if (this.hovered) {
      this.hovered = null;
      this.render();
    }
    this.cb.onHover?.(null, 0, 0, 0);
  };

  private onClick = (e: MouseEvent): void => {
    const { x, y } = this.local(e);
    const hit = this.hitTest(x, y);
    if (!hit || !this.root) return;
    if (hit.ancestor) {
      // Zoom out to the clicked ancestor (its index in the current path).
      const idx = this.path.indexOf(hit.node);
      if (idx >= 0) this.setFocus(hit.node, this.path.slice(0, idx + 1));
      return;
    }
    if (hit.node === this.focus) {
      // Clicking the focused frame zooms out one level.
      if (this.path.length > 1) {
        const p = this.path.slice(0, -1);
        this.setFocus(p[p.length - 1], p);
      }
      return;
    }
    // Zoom into a descendant: find its path from the current focus.
    const sub = findPath(this.focus as FlameNode, hit.node);
    if (sub) this.setFocus(hit.node, [...this.path.slice(0, -1), ...sub]);
  };

  resetFocus(): void {
    if (this.root) this.setFocus(this.root, [this.root]);
  }
}

// Path of nodes from `from` down to `target` (inclusive), or null.
function findPath(from: FlameNode, target: FlameNode): FlameNode[] | null {
  if (from === target) return [from];
  for (const c of from.children) {
    const p = findPath(c, target);
    if (p) return [from, ...p];
  }
  return null;
}
