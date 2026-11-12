// SPDX-License-Identifier: MIT
// Compound clock form for durations >= 1 minute: the two most significant
// units among d/h/m/s (e.g. 400000s -> "4d15h", 3661s -> "1h1m", 90s -> "1m30s").
export function formatClock(us: number): string {
  const neg = us < 0 ? "-" : "";
  let s = Math.round(Math.abs(us) / 1_000_000);
  const d = Math.floor(s / 86_400);
  s -= d * 86_400;
  const h = Math.floor(s / 3_600);
  s -= h * 3_600;
  const m = Math.floor(s / 60);
  s -= m * 60;
  if (d > 0) return `${neg}${d}d${h > 0 ? `${h}h` : ""}`;
  if (h > 0) return `${neg}${h}h${m > 0 ? `${m}m` : ""}`;
  if (m > 0) return `${neg}${m}m${s > 0 ? `${s}s` : ""}`;
  return `${neg}${s}s`;
}

// Format a microsecond value as a human-readable duration/time. Rolls up to
// minutes/hours/days past a minute so long traces don't read as "400000s".
export function formatTime(us: number): string {
  const a = Math.abs(us);
  if (a === 0) return "0";
  if (a < 1) return `${(us * 1000).toFixed(0)}ns`;
  if (a < 1000) return `${us.toFixed(a < 10 ? 2 : 0)}us`;
  if (a < 1_000_000) return `${(us / 1000).toFixed(a < 10_000 ? 2 : 1)}ms`;
  if (a < 60_000_000) return `${(us / 1_000_000).toFixed(a < 10_000_000 ? 3 : 2)}s`;
  return formatClock(us);
}

// Ruler tick label: below a minute, enough fractional digits that ticks spaced
// `step` us apart stay distinct (us/ms/s); at or above a minute, the compact
// d/h/m clock form so a wide axis reads in human units, not raw seconds.
export function formatTick(us: number, step: number): string {
  const a = Math.abs(us);
  if (a === 0) return "0";
  if (a >= 60_000_000) return formatClock(us);
  let div: number;
  let unit: string;
  if (a >= 1_000_000) {
    div = 1_000_000;
    unit = "s";
  } else if (a >= 1000) {
    div = 1000;
    unit = "ms";
  } else {
    div = 1;
    unit = "us";
  }
  const stepInUnit = step / div;
  const dec = stepInUnit >= 1 ? 0 : Math.min(6, Math.ceil(-Math.log10(stepInUnit)));
  return `${(us / div).toFixed(dec)}${unit}`;
}

// "4.80 GB" - for prose (panels, tooltips).
export function formatBytes(n: number): string {
  const u = ["B", "KB", "MB", "GB", "TB", "PB"];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) {
    n /= 1024;
    i++;
  }
  return `${n.toFixed(i === 0 ? 0 : n < 10 ? 2 : 1)} ${u[i]}`;
}

// "4.8G" - for the fixed-width gutter columns.
export function formatBytesCompact(n: number): string {
  const u = ["B", "K", "M", "G", "T", "P"];
  let i = 0;
  while (n >= 1024 && i < u.length - 1) {
    n /= 1024;
    i++;
  }
  return (i === 0 ? Math.round(n) : n.toFixed(n < 10 ? 1 : 0)) + u[i];
}

// "8.6k/s" - a per-second rate; "-" when zero.
export function formatRate(n: number): string {
  if (n <= 0) return "-";
  if (n >= 1e6) return `${(n / 1e6).toFixed(1)}M/s`;
  if (n >= 1e3) return `${(n / 1e3).toFixed(1)}k/s`;
  return `${Math.round(n)}/s`;
}

export function formatBytesPerSec(bps: number): string {
  if (bps < 1024) return `${bps.toFixed(0)} B/s`;
  if (bps < 1024 * 1024) return `${(bps / 1024).toFixed(1)} KB/s`;
  if (bps < 1024 * 1024 * 1024) return `${(bps / 1024 / 1024).toFixed(1)} MB/s`;
  return `${(bps / 1024 / 1024 / 1024).toFixed(2)} GB/s`;
}

// "8.6M", "1.2G" - compact SI magnitude for plain counter values (not bytes).
export function formatCompact(n: number): string {
  if (!Number.isFinite(n)) return "-";
  const a = Math.abs(n);
  if (a < 1000) return Number.isInteger(n) ? String(n) : n.toFixed(a < 10 ? 2 : 1);
  const u = ["", "K", "M", "G", "T", "P", "E"];
  let i = 0;
  let v = n;
  while (Math.abs(v) >= 1000 && i < u.length - 1) {
    v /= 1000;
    i++;
  }
  return `${v.toFixed(Math.abs(v) < 10 ? 2 : 1)}${u[i]}`;
}

// Choose a "nice" tick step (1/2/5 * 10^n) close to the target spacing.
export function niceStep(target: number): number {
  if (target <= 0) return 1;
  const pow = Math.pow(10, Math.floor(Math.log10(target)));
  const norm = target / pow;
  const mult = norm < 1.5 ? 1 : norm < 3.5 ? 2 : norm < 7.5 ? 5 : 10;
  return mult * pow;
}
