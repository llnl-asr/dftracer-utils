import { defineConfig } from "vitest/config";

// Unit tests cover pure modules (no DOM/JSX), so run them in the node
// environment and skip the app's Vite/Solid build plugins.
export default defineConfig({
  test: {
    environment: "node",
    include: ["src/**/*.test.ts"],
  },
});
