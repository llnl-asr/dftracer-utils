import type { FlameNode } from "../data/types";

export interface FnRow {
  name: string;
  self: number;
  total: number;
  count: number;
}

function emptyNode(name: string): FlameNode {
  return { name, total: 0, self: 0, count: 0, children: [] };
}

// The functions to aggregate live directly under each node in `roots`; a root
// is a synthetic container (the whole-trace root, or a per-process frame) and is
// itself excluded. Passing multiple roots aggregates across them (e.g. all
// processes), passing one scopes to it (a single process).

// Aggregate every function across all its call sites: self always sums; total
// sums only at the top-most occurrence on a branch so recursion is not double
// counted; count sums all occurrences. Iterative so large trees can be walked
// in bounded chunks without blocking the main thread.
class FnListBuilder {
  private map = new Map<string, FnRow>();
  private ancest = new Set<string>();
  private stack: { n: FlameNode; i: number; added: string | null }[];

  constructor(roots: FlameNode[]) {
    this.stack = roots.map((r) => ({ n: r, i: 0, added: null as string | null })).reverse();
  }

  // Process up to `budget` steps; true when the walk is complete.
  step(budget: number): boolean {
    const { map, ancest, stack } = this;
    while (budget-- > 0 && stack.length) {
      const top = stack[stack.length - 1];
      if (top.i < top.n.children.length) {
        const c = top.n.children[top.i++];
        let r = map.get(c.name);
        if (!r) {
          r = { name: c.name, self: 0, total: 0, count: 0 };
          map.set(c.name, r);
        }
        r.self += c.self;
        r.count += c.count;
        const nested = ancest.has(c.name);
        if (!nested) {
          r.total += c.total;
          ancest.add(c.name);
        }
        stack.push({ n: c, i: 0, added: nested ? null : c.name });
      } else {
        stack.pop();
        if (top.added !== null) ancest.delete(top.added);
      }
    }
    return stack.length === 0;
  }

  result(): FnRow[] {
    return [...this.map.values()];
  }
}

export function functionList(roots: FlameNode[]): FnRow[] {
  const b = new FnListBuilder(roots);
  while (!b.step(1 << 20)) {
    /* drain synchronously */
  }
  return b.result();
}

// Chunked variant: yields to the event loop between chunks so the UI stays
// responsive on huge trees. `isStale` aborts a superseded computation.
export async function functionListAsync(
  roots: FlameNode[],
  isStale?: () => boolean,
): Promise<FnRow[]> {
  const b = new FnListBuilder(roots);
  while (!b.step(50000)) {
    if (isStale?.()) return [];
    await new Promise<void>((resolve) => setTimeout(resolve, 0));
  }
  return b.result();
}

// Merge every subtree rooted at `name` into one tree (the callees view).
export function calleesTree(roots: FlameNode[], name: string): FlameNode {
  const merged = emptyNode(name);
  const idx = new Map<FlameNode, Map<string, FlameNode>>();
  const mergeInto = (dst: FlameNode, src: FlameNode): void => {
    dst.total += src.total;
    dst.self += src.self;
    dst.count += src.count;
    for (const sc of src.children) {
      let m = idx.get(dst);
      if (!m) {
        m = new Map();
        idx.set(dst, m);
      }
      let dc = m.get(sc.name);
      if (!dc) {
        dc = emptyNode(sc.name);
        m.set(sc.name, dc);
        dst.children.push(dc);
      }
      mergeInto(dc, sc);
    }
  };
  const find = (n: FlameNode, inside: boolean): void => {
    for (const c of n.children) {
      if (c.name === name && !inside) {
        mergeInto(merged, c);
        find(c, true);
      } else {
        find(c, inside);
      }
    }
  };
  for (const root of roots) find(root, false);
  return merged;
}

// Inverted tree rooted at `name` whose children are its callers, weighted by the
// time that flowed through `name` under each caller (the callers view).
export function callersTree(roots: FlameNode[], name: string): FlameNode {
  const merged = emptyNode(name);
  const idx = new Map<FlameNode, Map<string, FlameNode>>();
  const stack: FlameNode[] = [];
  const walk = (n: FlameNode, inside: boolean): void => {
    for (const c of n.children) {
      const match = c.name === name && !inside;
      if (match) {
        merged.total += c.total;
        merged.count += c.count;
        let dst = merged;
        for (let i = stack.length - 1; i >= 0; i--) {
          const anc = stack[i];
          let m = idx.get(dst);
          if (!m) {
            m = new Map();
            idx.set(dst, m);
          }
          let dc = m.get(anc.name);
          if (!dc) {
            dc = emptyNode(anc.name);
            m.set(anc.name, dc);
            dst.children.push(dc);
          }
          dc.total += c.total;
          dc.count += c.count;
          dst = dc;
        }
      }
      stack.push(c);
      walk(c, inside || match);
      stack.pop();
    }
  };
  for (const root of roots) {
    stack.length = 0;
    walk(root, false);
  }
  return merged;
}
