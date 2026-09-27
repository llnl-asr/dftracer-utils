import { describe, expect, it } from "vitest";
import { entityClause, laneClause, setEntityLabels, setSchema, textClause } from "./fields";

describe("fields", () => {
  it("names dftracer's fields by default", () => {
    expect(laneClause("12", "3")).toBe("(pid == 12 and tid == 3)");
    expect(entityClause("12")).toBe("pid == 12");
    expect(textClause("cat", "POSIX")).toBe('cat == "POSIX"');
  });

  it("names a path schema's roles and its text entity values", () => {
    setSchema({
      id: "weblog",
      decoder: "path",
      fields: {
        time: "t",
        duration: "took",
        entity: "client",
        lane: "",
        label: "req",
        entity_name: "client",
      },
    });
    setEntityLabels(new Map([["1234", "10.0.0.1"]]));
    expect(laneClause("1234", "0")).toBe('client == "10.0.0.1"');
    expect(laneClause("7", "0")).toBe("client == 7");
    expect(entityClause("10.0.0.1")).toBe('client == "10.0.0.1"');
    expect(textClause("name", 'GET "/a"')).toBe('req == "GET \\"/a\\""');
    expect(textClause("cat", "x")).toBeNull();
  });
});
