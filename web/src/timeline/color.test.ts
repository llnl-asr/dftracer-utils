import { describe, expect, it } from "vitest";
import { colorFor, colorSlot, DENSITY_GREY } from "./color";

const SEP = colorSlot("a", "b").slice(1, -1);

describe("colorSlot", () => {
  it("namespaces a value by its field", () => {
    expect(colorSlot("cat", "POSIX")).toBe(`cat${SEP}POSIX`);
    expect(colorSlot("pid", "99")).toBe(`pid${SEP}99`);
  });

  it("passes the value through when no field (name mode)", () => {
    expect(colorSlot("", "read")).toBe("read");
  });

  it("keeps the same value in different fields distinct in the palette", () => {
    const a = colorFor(colorSlot("cat", "0"));
    const b = colorFor(colorSlot("ret", "0"));
    // Distinct palette keys, so switching fields never collides on a shared key.
    expect(colorSlot("cat", "0")).not.toBe(colorSlot("ret", "0"));
    expect(typeof a).toBe("string");
    expect(typeof b).toBe("string");
  });

  it("exposes a neutral grey for folded density blocks", () => {
    expect(DENSITY_GREY).toMatch(/^#[0-9a-f]{6}$/i);
  });
});

describe("colorFor overflow", () => {
  it("hands out unique valid hex colors well past the curated palette", () => {
    const seen = new Set<string>();
    for (let i = 0; i < 64; i++) {
      const c = colorFor(`overflow-key-${i}`);
      expect(c).toMatch(/^#[0-9a-f]{6}$/i);
      seen.add(c);
    }
    // 64 distinct entities should not collapse onto the 16-slot palette.
    expect(seen.size).toBeGreaterThan(40);
  });
});
