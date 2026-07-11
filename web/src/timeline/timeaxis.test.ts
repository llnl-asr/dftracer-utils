import { describe, expect, it } from "vitest";

import { buildTimeAxis } from "./timeaxis";

describe("buildTimeAxis", () => {
  it("is the identity when disabled", () => {
    const ax = buildTimeAxis([{ begin: 50, end: 150 }], 200, false);
    expect(ax.enabled).toBe(false);
    expect(ax.displayTotal).toBe(200);
    expect(ax.breakUs).toBe(0);
    for (const t of [0, 37, 150, 200]) {
      expect(ax.toDisplay(t)).toBe(t);
      expect(ax.toReal(t)).toBe(t);
    }
  });

  it("is the identity when there are no gaps", () => {
    const ax = buildTimeAxis([], 200, true);
    expect(ax.enabled).toBe(false);
    expect(ax.displayTotal).toBe(200);
    expect(ax.toDisplay(123)).toBe(123);
  });

  describe("with one gap (runA [0,50], idle [50,150], runB [150,200])", () => {
    const realTotal = 200;
    const ax = buildTimeAxis([{ begin: 50, end: 150 }], realTotal, true);

    it("collapses the gap so the total shrinks by nearly its width", () => {
      expect(ax.enabled).toBe(true);
      // active = 100, break = 3% of active = 3 -> displayTotal = 100 + 3
      expect(ax.breakUs).toBeCloseTo(3, 6);
      expect(ax.displayTotal).toBeCloseTo(103, 6);
    });

    it("pins the endpoints", () => {
      expect(ax.toDisplay(0)).toBe(0);
      expect(ax.toDisplay(realTotal)).toBeCloseTo(ax.displayTotal, 6);
      expect(ax.toReal(0)).toBe(0);
      expect(ax.toReal(ax.displayTotal)).toBeCloseTo(realTotal, 6);
    });

    it("keeps active spans at slope 1", () => {
      expect(ax.toDisplay(25)).toBeCloseTo(25, 6); // inside run A
      expect(ax.toDisplay(50)).toBeCloseTo(50, 6); // gap start
      expect(ax.toDisplay(150)).toBeCloseTo(53, 6); // gap end = 50 + break
      expect(ax.toDisplay(175)).toBeCloseTo(78, 6); // 25 into run B
    });

    it("round-trips real -> display -> real on active points", () => {
      for (const t of [0, 10, 49.9, 150, 175, 200]) {
        expect(ax.toReal(ax.toDisplay(t))).toBeCloseTo(t, 6);
      }
    });

    it("is monotonically non-decreasing", () => {
      let prev = -Infinity;
      for (let t = 0; t <= realTotal; t += 1) {
        const d = ax.toDisplay(t);
        expect(d).toBeGreaterThanOrEqual(prev);
        prev = d;
      }
    });

    it("brings run B adjacent to run A (the whole gap is skipped visually)", () => {
      // Run B's first display px sits just past run A's last, separated only by
      // the break width - not by the 100us of real idle time.
      const runAEnd = ax.toDisplay(50);
      const runBStart = ax.toDisplay(150);
      expect(runBStart - runAEnd).toBeCloseTo(ax.breakUs, 6);
    });
  });

  it("handles multiple gaps and stays monotonic and invertible", () => {
    const gaps = [
      { begin: 100, end: 500 },
      { begin: 700, end: 900 },
    ];
    const ax = buildTimeAxis(gaps, 1000, true);
    expect(ax.enabled).toBe(true);
    // active = 1000 - 600 = 400, two breaks -> displayTotal = 400 + 2*break
    expect(ax.displayTotal).toBeCloseTo(400 + 2 * ax.breakUs, 6);
    for (const t of [0, 50, 100, 500, 650, 900, 1000]) {
      expect(ax.toReal(ax.toDisplay(t))).toBeCloseTo(t, 6);
    }
  });

  it("sorts unsorted gaps before building", () => {
    const ax = buildTimeAxis(
      [
        { begin: 700, end: 900 },
        { begin: 100, end: 500 },
      ],
      1000,
      true,
    );
    expect(ax.gaps[0].begin).toBe(100);
    expect(ax.gaps[1].begin).toBe(700);
    expect(ax.toDisplay(0)).toBe(0);
    expect(ax.toDisplay(1000)).toBeCloseTo(ax.displayTotal, 6);
  });

  it("clamps out-of-range inputs to the endpoints", () => {
    const ax = buildTimeAxis([{ begin: 50, end: 150 }], 200, true);
    expect(ax.toDisplay(-10)).toBe(0);
    expect(ax.toDisplay(9999)).toBeCloseTo(ax.displayTotal, 6);
    expect(ax.toReal(-10)).toBe(0);
    expect(ax.toReal(9999)).toBeCloseTo(200, 6);
  });
});
