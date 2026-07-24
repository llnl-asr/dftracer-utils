import { describe, expect, it } from "vitest";
import type { TraceEvent } from "../data/types";
import {
  collectGroupColumns,
  compareGroupKeys,
  eventGroupValue,
  NONE_GROUP,
  resolveGroup,
  takeRow,
} from "./timeline";

function ev(extra: Record<string, unknown>): TraceEvent {
  return {
    name: "read",
    cat: "POSIX",
    pid: 1,
    tid: 2,
    ts: 0,
    dur: 5,
    ph: "X",
    args: { ret: 64, level: 0, empty: "", flag: true, nested: { deep: "x" } },
    ...extra,
  } as unknown as TraceEvent;
}

describe("eventGroupValue", () => {
  it("reads top-level columns", () => {
    expect(eventGroupValue(ev({}), "cat")).toBe("POSIX");
    expect(eventGroupValue(ev({}), "name")).toBe("read");
  });

  it("falls back into args for bare names", () => {
    expect(eventGroupValue(ev({}), "ret")).toBe("64");
    expect(eventGroupValue(ev({}), "flag")).toBe("true");
  });

  it("walks dotted paths", () => {
    expect(eventGroupValue(ev({}), "args.ret")).toBe("64");
    expect(eventGroupValue(ev({}), "args.nested.deep")).toBe("x");
  });

  it("groups missing columns under (none)", () => {
    expect(eventGroupValue(ev({}), "does_not_exist")).toBe(NONE_GROUP);
    expect(eventGroupValue(ev({}), "args.nope")).toBe(NONE_GROUP);
    expect(eventGroupValue(ev({}), "a.b.c.d")).toBe(NONE_GROUP);
  });

  it("groups null, empty, and non-scalar values under (none)", () => {
    expect(eventGroupValue(ev({ cat: null }), "cat")).toBe(NONE_GROUP);
    expect(eventGroupValue(ev({ cat: undefined }), "cat")).toBe(NONE_GROUP);
    expect(eventGroupValue(ev({}), "empty")).toBe(NONE_GROUP);
    expect(eventGroupValue(ev({}), "args")).toBe(NONE_GROUP); // object
    expect(eventGroupValue(ev({}), "nested")).toBe(NONE_GROUP);
  });

  it("handles events with no args at all", () => {
    const bare = { name: "x", pid: 1, tid: 1, ts: 0, dur: 1, ph: "X" } as unknown as TraceEvent;
    expect(eventGroupValue(bare, "ret")).toBe(NONE_GROUP);
    expect(eventGroupValue(bare, "args.ret")).toBe(NONE_GROUP);
    expect(eventGroupValue(bare, "name")).toBe("x");
  });

  it("stringifies numeric values", () => {
    expect(eventGroupValue(ev({ pid: 7 }), "pid")).toBe("7");
    expect(eventGroupValue(ev({}), "level")).toBe("0");
  });

  it("joins multiple columns into a composite key", () => {
    expect(eventGroupValue(ev({}), "cat,ret")).toBe("POSIX\x1f64");
    expect(eventGroupValue(ev({}), "cat,name,level")).toBe("POSIX\x1fread\x1f0");
  });

  it("keeps missing composite components empty (matches server)", () => {
    expect(eventGroupValue(ev({}), "cat,nope")).toBe("POSIX\x1f");
    expect(eventGroupValue(ev({}), "nope,ret")).toBe("\x1f64");
  });
});

describe("resolveGroup", () => {
  it("maps resolved names and passes unknown values through", () => {
    const names = { abc123: "node01", fh1: "/data/train.h5" };
    expect(resolveGroup("abc123", names)).toBe("node01");
    expect(resolveGroup("fh1", names)).toBe("/data/train.h5");
    expect(resolveGroup("unknown-hash", names)).toBe("unknown-hash");
    expect(resolveGroup("(none)", names)).toBe("(none)");
  });

  it("passes through without a map", () => {
    expect(resolveGroup("abc123")).toBe("abc123");
  });

  it("resolves each component of a composite key and joins for display", () => {
    const names = { fh1: "/data/train.h5", h2: "node01" };
    expect(resolveGroup("POSIX\x1ffh1", names)).toBe("POSIX / /data/train.h5");
    expect(resolveGroup("h2\x1ffh1", names)).toBe("node01 / /data/train.h5");
    expect(resolveGroup("POSIX\x1funknown", names)).toBe("POSIX / unknown");
    expect(resolveGroup("POSIX\x1f", names)).toBe("POSIX / (none)");
  });
});

describe("collectGroupColumns", () => {
  it("collects scalar top-level and args columns with resolved aliases", () => {
    const into = new Set<string>();
    collectGroupColumns(
      [ev({}), ev({ args: { fhash: "h1", ret: 1 } }), ev({ args: { hhash: "h2" } })],
      into,
    );
    expect(into.has("cat")).toBe(true);
    expect(into.has("name")).toBe(true);
    expect(into.has("ret")).toBe(true);
    expect(into.has("fhash")).toBe(true);
    expect(into.has("resolved.fpath")).toBe(true);
    expect(into.has("resolved.hostname")).toBe(true);
    // lane levels / bookkeeping fields are not suggestions
    expect(into.has("pid")).toBe(false);
    expect(into.has("ts")).toBe(false);
    expect(into.has("args")).toBe(false);
  });

  it("skips null, empty, and object values, and handles missing args", () => {
    const into = new Set<string>();
    const bare = { name: "x", pid: 1, tid: 1, ts: 0, dur: 1, ph: "X" } as unknown as TraceEvent;
    collectGroupColumns([bare, ev({ cat: null, args: { empty: "", nested: { a: 1 } } })], into);
    expect(into.has("name")).toBe(true);
    expect(into.has("empty")).toBe(false);
    expect(into.has("nested")).toBe(false);
  });
});

describe("takeRow", () => {
  it("packs staggered overlapping spans into max-concurrency rows", () => {
    // 100 jobs, one every 10s, each 30s long: concurrency 3, not 100 rows.
    const rows: number[] = [];
    let maxRow = 0;
    for (let i = 0; i < 100; i++) maxRow = Math.max(maxRow, takeRow(rows, i * 10, i * 10 + 30));
    expect(maxRow).toBe(2);
    expect(rows.length).toBe(3);
  });

  it("keeps proper nesting on separate rows", () => {
    const rows: number[] = [];
    expect(takeRow(rows, 0, 100)).toBe(0); // parent
    expect(takeRow(rows, 10, 50)).toBe(1); // child
    expect(takeRow(rows, 20, 30)).toBe(2); // grandchild
    expect(takeRow(rows, 60, 90)).toBe(1); // sibling reuses the child's row
  });

  it("reuses a freed row for disjoint spans", () => {
    const rows: number[] = [];
    expect(takeRow(rows, 0, 10)).toBe(0);
    expect(takeRow(rows, 10, 20)).toBe(0);
    expect(takeRow(rows, 15, 25)).toBe(1);
  });
});

describe("compareGroupKeys", () => {
  it("orders numeric-looking values numerically, not lexically", () => {
    const vals = ["10", "2", "0", "20", "1"];
    expect([...vals].sort(compareGroupKeys)).toEqual(["0", "1", "2", "10", "20"]);
  });

  it("orders numbers before text and text naturally", () => {
    const vals = ["node10", "5", "node2", "1"];
    expect([...vals].sort(compareGroupKeys)).toEqual(["1", "5", "node2", "node10"]);
  });

  it("handles negative and float values", () => {
    expect([...["1.5", "-3", "0.25"]].sort(compareGroupKeys)).toEqual(["-3", "0.25", "1.5"]);
  });
});
