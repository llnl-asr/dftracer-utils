// SPDX-License-Identifier: MIT
import { describe, expect, it } from "vitest";
import { planAggregate } from "./aggregate";

describe("planAggregate", () => {
  it("collapses to a block when the window is barely visible", () => {
    expect(planAggregate(2, 100, 0.5).mode).toBe("block");
  });

  it("collapses to a block for a singleton aggregate", () => {
    expect(planAggregate(500, 1, 1).mode).toBe("block");
  });

  it("draws an estimated band when events outnumber pixels", () => {
    expect(planAggregate(50, 4_000_000, 0.2).mode).toBe("band");
  });

  it("places uniformly-spaced marks with room to spare", () => {
    // 1s busy in a 5s window shown 500px wide, 10 events -> the user's example:
    // one 100ms mark every 500ms, i.e. 50px apart, each 10px wide.
    const p = planAggregate(500, 10, 1 / 5);
    expect(p.mode).toBe("marks");
    expect(p.n).toBe(10);
    expect(p.spacing).toBeCloseTo(50);
    expect(p.markW).toBeCloseTo(10);
  });

  it("never draws more marks than events", () => {
    const p = planAggregate(500, 10, 1);
    expect(p.n).toBe(10);
  });

  it("clamps a degenerate busy fraction to a full slot", () => {
    const p = planAggregate(100, 10, 0);
    expect(p.mode).toBe("marks");
    expect(p.markW).toBeCloseTo(10);
  });
});
