// Curated qualitative palette tuned for the dark timeline surface. Ordered for
// separation between neighbours; assigned to names in first-seen order so a name
// keeps its color across queries (color follows the entity, not its rank).
const PALETTE = [
  "#4bb0c4", // teal (read)
  "#e6a13c", // amber (write)
  "#5f7d52", // olive (compute)
  "#b06fc4", // orchid (meta)
  "#d1524e", // red (sync)
  "#5b8fd1", // blue (mpi)
  "#9aa84e", // olive-yellow (seek)
  "#c98a5a", // clay
  "#6fb3a0", // sage
  "#c76f96", // dusty rose
  "#8f9bd6", // periwinkle
  "#b3924e", // bronze
  "#7fb36a", // leaf
  "#d0a24e", // wheat
  "#9a7bc4", // muted violet
  "#6aa9bf", // steel
];

// Neutral fill for folded density blocks when coloring by a field: a folded
// block aggregates many events, so a single hue would misrepresent the mix.
export const DENSITY_GREY = "#4a515e";

// Palette key for a color-by-field value. Namespacing by field keeps color
// assignment stable per value and avoids cross-field collisions in the shared
// palette map.
export function colorSlot(field: string, value: string): string {
  return field ? `${field}\u0000${value}` : value;
}

const assigned = new Map<string, string>();
let nextSlot = 0;

function hslToHex(h: number, s: number, l: number): string {
  l /= 100;
  const a = (s * Math.min(l, 1 - l)) / 100;
  const f = (n: number) => {
    const k = (n + h / 30) % 12;
    const c = l - a * Math.max(-1, Math.min(k - 3, 9 - k, 1));
    return Math.round(255 * c)
      .toString(16)
      .padStart(2, "0");
  };
  return `#${f(0)}${f(8)}${f(4)}`;
}

// Beyond the curated palette, sweep the hue wheel by the golden angle so each
// new entity gets a maximally-separated, unique color instead of wrapping and
// silently colliding. Lightness alternates in two bands for extra separation
// between near-adjacent hues.
function generatedColor(slot: number): string {
  const overflow = slot - PALETTE.length;
  const hue = (overflow * 137.508) % 360;
  const light = overflow % 2 === 0 ? 62 : 50;
  return hslToHex(hue, 52, light);
}

export function colorFor(key: string): string {
  let c = assigned.get(key);
  if (c) return c;
  c = nextSlot < PALETTE.length ? PALETTE[nextSlot] : generatedColor(nextSlot);
  nextSlot += 1;
  assigned.set(key, c);
  return c;
}

// Pick readable label ink for a given slice fill (dark on bright hues, light on
// dark hues).
export function contrastText(hex: string): string {
  const h = hex.replace("#", "");
  const r = parseInt(h.slice(0, 2), 16);
  const g = parseInt(h.slice(2, 4), 16);
  const b = parseInt(h.slice(4, 6), 16);
  const lum = (0.299 * r + 0.587 * g + 0.114 * b) / 255;
  return lum > 0.6 ? "rgba(10,12,18,0.95)" : "rgba(255,255,255,0.95)";
}

// Color/legend key for an event: name first (what varies per slice), falling
// back to category. Matches Perfetto's per-name coloring.
export function sliceKey(ev: { name?: unknown; cat?: unknown }): string {
  const name = ev.name != null ? String(ev.name) : "";
  const cat = ev.cat != null ? String(ev.cat) : "";
  return name || cat;
}
