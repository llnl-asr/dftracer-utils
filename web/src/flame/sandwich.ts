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
// counted; count sums all occurrences.
export function functionList(roots: FlameNode[]): FnRow[] {
  const map = new Map<string, FnRow>();
  const ancest = new Set<string>();
  const walk = (n: FlameNode): void => {
    for (const c of n.children) {
      let r = map.get(c.name);
      if (!r) {
        r = { name: c.name, self: 0, total: 0, count: 0 };
        map.set(c.name, r);
      }
      r.self += c.self;
      r.count += c.count;
      const nested = ancest.has(c.name);
      if (!nested) r.total += c.total;
      if (!nested) ancest.add(c.name);
      walk(c);
      if (!nested) ancest.delete(c.name);
    }
  };
  for (const root of roots) walk(root);
  return [...map.values()];
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
