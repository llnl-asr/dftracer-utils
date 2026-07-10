import { defineConfig } from "vite";
import solid from "vite-plugin-solid";
import { viteSingleFile } from "vite-plugin-singlefile";

// Emit self-contained pages (all JS/CSS inlined) that the C++ build embeds into
// dftracer_server. vite-plugin-singlefile forces one chunk, so each page is a
// separate build: `vite build` -> dist/index.html (viewer), `DFT_PAGE=api vite
// build` -> dist/api.html (API explorer). `npm run build` runs both.
const page = process.env.DFT_PAGE === "api" ? "api" : "index";

export default defineConfig({
  plugins: [solid(), viteSingleFile()],
  server: {
    proxy: {
      "/api": "http://localhost:8080",
    },
  },
  build: {
    target: "es2020",
    outDir: "dist",
    emptyOutDir: page === "index", // only the first build clears dist/
    cssCodeSplit: false,
    assetsInlineLimit: 100000000,
    chunkSizeWarningLimit: 100000,
    rollupOptions: {
      input: page === "api" ? "api.html" : "index.html",
    },
  },
});
