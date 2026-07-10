export type ThemeMode = "dark" | "light";

export interface VizTheme {
  plotBg: string;
  panelBg: string;
  grid: string;
  divider: string;
  laneAlt: string;
  groupBand: string;
  groupText: string;
  laneText: string;
  numText: string;
  ruler: string;
  read: string;
  write: string;
  counterLabel: string;
  miniBg: string;
  miniIdle: string;
  miniActivity: string;
  accent: string;
  accentSoft: string;
  selFill: string;
  selStroke: string;
  gapFill: string;
  gapActiveFill: string;
  gapStroke: string;
  ttBg: string;
  ttBorder: string;
  ttName: string;
  ttHot: string;
  crosshair: string;
  crosshairText: string;
  flameAncestor: string;
  flameAncestorText: string;
  flameLabelDark: string;
}

const DARK: VizTheme = {
  plotBg: "#0a0b0a",
  panelBg: "#0c0e0b",
  grid: "#16180f",
  divider: "#1f2119",
  laneAlt: "#0d0f0c",
  groupBand: "#12140f",
  groupText: "#e6e2ce",
  laneText: "#8a927c",
  numText: "#8a927c",
  ruler: "#7d876b",
  read: "#4bb0c4",
  write: "#e6a13c",
  counterLabel: "#d6d3c0",
  miniBg: "#0a0b0a",
  miniIdle: "rgba(255,255,255,0.03)",
  miniActivity: "120,164,150",
  accent: "#e6a13c",
  accentSoft: "rgba(230,161,60,0.16)",
  selFill: "rgba(230,161,60,0.14)",
  selStroke: "rgba(230,161,60,0.9)",
  gapFill: "rgba(209,82,78,0.16)",
  gapActiveFill: "rgba(209,82,78,0.34)",
  gapStroke: "rgba(209,82,78,0.95)",
  ttBg: "#12140f",
  ttBorder: "#3a3d30",
  ttName: "#e6e2ce",
  ttHot: "#e6a13c",
  crosshair: "rgba(230,161,60,0.35)",
  crosshairText: "#e6a13c",
  flameAncestor: "#2a2d24",
  flameAncestorText: "#c9c5b2",
  flameLabelDark: "#0a0b0a",
};

const LIGHT: VizTheme = {
  plotBg: "#fffdf7",
  panelBg: "#f1ece2",
  grid: "#f0eadd",
  divider: "#e2dccd",
  laneAlt: "#faf7ef",
  groupBand: "#f3eee4",
  groupText: "#2a2620",
  laneText: "#6f6656",
  numText: "#8a8272",
  ruler: "#a89f8c",
  read: "#2c8296",
  write: "#bf7d1e",
  counterLabel: "#2a2620",
  miniBg: "#f3eee4",
  miniIdle: "rgba(0,0,0,0.04)",
  miniActivity: "120,90,40",
  accent: "#c2740f",
  accentSoft: "rgba(194,116,15,0.16)",
  selFill: "rgba(194,116,15,0.13)",
  selStroke: "rgba(194,116,15,0.85)",
  gapFill: "rgba(192,57,43,0.14)",
  gapActiveFill: "rgba(192,57,43,0.28)",
  gapStroke: "rgba(192,57,43,0.85)",
  ttBg: "#fffdf7",
  ttBorder: "#ddd6c8",
  ttName: "#2a2620",
  ttHot: "#bf7d1e",
  crosshair: "rgba(194,116,15,0.3)",
  crosshairText: "#bf7d1e",
  flameAncestor: "#e8e1d2",
  flameAncestorText: "#4a4438",
  flameLabelDark: "#2a2620",
};

export function vizTheme(mode: ThemeMode): VizTheme {
  return mode === "light" ? LIGHT : DARK;
}
