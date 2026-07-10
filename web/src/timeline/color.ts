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

const assigned = new Map<string, string>();
let nextSlot = 0;

export function colorFor(key: string): string {
  let c = assigned.get(key);
  if (c) return c;
  c = PALETTE[nextSlot % PALETTE.length];
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
