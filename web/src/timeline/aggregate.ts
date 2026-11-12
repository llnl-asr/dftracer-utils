// SPDX-License-Identifier: MIT
// Render plan for a DFTracer aggregated (ph=3) event. Its individual events were
// dropped at capture time, so their positions inside the window are unknown.
// When the window is wide enough on screen we place `count` uniformly-spaced
// synthetic marks (an estimate); when there are more events than pixels we fall
// back to an estimated band; when the window is barely visible, a merged block.

export type AggMode = "block" | "marks" | "band";

export interface AggPlan {
  mode: AggMode;
  n: number; // marks to draw (marks mode)
  spacing: number; // px between mark starts (marks mode)
  markW: number; // px width of one mark (marks mode)
}

// Below this on-screen window width the marks would not be legible, so the
// window collapses to a single merged block.
export const AGG_MIN_PX = 3;

// pxW: on-screen width of the aggregation window, in pixels.
// count: dft_cnt, number of collapsed events.
// busyFrac: dur_sum / dur in (0, 1]; each mark occupies this fraction of its slot.
export function planAggregate(pxW: number, count: number, busyFrac: number): AggPlan {
  const block: AggPlan = { mode: "block", n: 0, spacing: 0, markW: 0 };
  if (!(pxW > 0) || count <= 1 || pxW < AGG_MIN_PX) return block;
  if (count > pxW) return { mode: "band", n: 0, spacing: 0, markW: 0 };
  const spacing = pxW / count;
  const frac = busyFrac > 0 && busyFrac <= 1 ? busyFrac : 1;
  return { mode: "marks", n: count, spacing, markW: Math.max(1, frac * spacing) };
}
