import { describe, expect, it } from "vitest";
import type { FlameNode } from "../data/types";
import { functionList, functionListAsync, type FnRow } from "./sandwich";

// The original recursive walk, kept as the reference implementation.
function referenceFunctionList(roots: FlameNode[]): FnRow[] {
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

// Deterministic pseudo-random tree with repeated names to exercise the
// recursion (ancestor) handling.
function makeTree(seed: number, depth: number, breadth: number): FlameNode {
  let s = seed;
  const rand = () => {
    s = (s * 1103515245 + 12345) % 2147483648;
    return s / 2147483648;
  };
  const names = ["read", "write", "open", "close", "compute", "mpi", "epoch"];
  const build = (d: number): FlameNode => {
    const name = names[Math.floor(rand() * names.length)];
    const kids: FlameNode[] = [];
    if (d < depth) {
      const n = 1 + Math.floor(rand() * breadth);
      for (let i = 0; i < n; i++) kids.push(build(d + 1));
    }
    const childTotal = kids.reduce((a, c) => a + c.total, 0);
    const self = Math.floor(rand() * 100);
    return { name, total: childTotal + self, self, count: 1 + kids.length, children: kids };
  };
  const root = build(0);
  root.name = "all";
  return root;
}

describe("functionList", () => {
  it("matches the reference recursive implementation", () => {
    for (const seed of [1, 42, 999]) {
      const tree = makeTree(seed, 6, 3);
      expect(functionList([tree])).toEqual(referenceFunctionList([tree]));
    }
  });

  it("aggregates across multiple roots like the reference", () => {
    const a = makeTree(7, 5, 3);
    const b = makeTree(13, 5, 3);
    expect(functionList([a, b])).toEqual(referenceFunctionList([a, b]));
  });

  it("handles an empty tree", () => {
    const empty: FlameNode = { name: "all", total: 0, self: 0, count: 0, children: [] };
    expect(functionList([empty])).toEqual([]);
  });

  it("async variant produces the same result", async () => {
    const tree = makeTree(42, 7, 3);
    expect(await functionListAsync([tree])).toEqual(functionList([tree]));
  });

  it("async variant aborts when stale", async () => {
    // Staleness is only checked between chunks, so the tree must exceed one
    // chunk budget (50k steps; ~2 steps per node).
    const kids: FlameNode[] = Array.from({ length: 60000 }, (_, i) => ({
      name: `f${i % 50}`,
      total: 1,
      self: 1,
      count: 1,
      children: [],
    }));
    const tree: FlameNode = { name: "all", total: 60000, self: 0, count: 1, children: kids };
    expect(await functionListAsync([tree], () => true)).toEqual([]);
  });
});
