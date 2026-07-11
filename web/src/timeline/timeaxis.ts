// Timelapse axis: a monotonic piecewise-linear map from real microseconds to
// the compressed "display" microseconds the timeline lays out in. Active spans
// keep slope 1; each between-run idle gap collapses to a fixed break width so
// the runs sit together. Pure and canvas-free so it can be unit tested.

export interface AxisGap {
  begin: number;
  end: number;
}

export interface TimeAxis {
  readonly enabled: boolean;
  readonly displayTotal: number;
  readonly breakUs: number;
  readonly gaps: AxisGap[];
  toDisplay(real: number): number;
  toReal(display: number): number;
}

const MIN_SPAN = 1;
// Each gap collapses to this fraction of the total active time (split across
// gaps), leaving a visible but small break marker.
const BREAK_FRAC = 0.03;

export function buildTimeAxis(gaps: AxisGap[], realTotal: number, enabled: boolean): TimeAxis {
  const rt = Math.max(realTotal, MIN_SPAN);
  const sorted = [...gaps].sort((a, b) => a.begin - b.begin);

  if (!enabled || sorted.length === 0) {
    return {
      enabled: false,
      displayTotal: rt,
      breakUs: 0,
      gaps: sorted,
      toDisplay: (real) => real,
      toReal: (display) => display,
    };
  }

  let gapSum = 0;
  for (const g of sorted) gapSum += Math.max(0, g.end - g.begin);
  const active = Math.max(MIN_SPAN, rt - gapSum);
  const breakUs = (active * BREAK_FRAC) / sorted.length;

  // Knots (real -> display) at each active/gap boundary; between them the map
  // is linear, so both directions interpolate with one scan.
  const knots: { r: number; d: number }[] = [{ r: 0, d: 0 }];
  let d = 0;
  let rprev = 0;
  for (const g of sorted) {
    const b = Math.max(rprev, g.begin);
    const e = Math.max(b, g.end);
    d += b - rprev;
    knots.push({ r: b, d });
    d += breakUs;
    knots.push({ r: e, d });
    rprev = e;
  }
  d += rt - rprev;
  knots.push({ r: rt, d });

  const toDisplay = (real: number): number => {
    if (real <= knots[0].r) return knots[0].d;
    for (let i = 1; i < knots.length; i++) {
      if (real <= knots[i].r) {
        const dr = knots[i].r - knots[i - 1].r;
        const f = dr > 0 ? (real - knots[i - 1].r) / dr : 0;
        return knots[i - 1].d + f * (knots[i].d - knots[i - 1].d);
      }
    }
    return knots[knots.length - 1].d;
  };

  const toReal = (display: number): number => {
    if (display <= knots[0].d) return knots[0].r;
    for (let i = 1; i < knots.length; i++) {
      if (display <= knots[i].d) {
        const dd = knots[i].d - knots[i - 1].d;
        const f = dd > 0 ? (display - knots[i - 1].d) / dd : 0;
        return knots[i - 1].r + f * (knots[i].r - knots[i - 1].r);
      }
    }
    return knots[knots.length - 1].r;
  };

  return { enabled: true, displayTotal: d, breakUs, gaps: sorted, toDisplay, toReal };
}
