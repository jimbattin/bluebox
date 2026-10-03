import { expect, test } from "bun:test";
import { PAGE_KEY_SLOTS, PROGRAMS } from "./programs";

// A page missing from its family's keys could never be selected in the editor.
test("every program page has a key in its family, and the keys fit the slots", () => {
  for (const prog of PROGRAMS) {
    const keyPages = prog.pageKeys.map((k) => k.page);
    expect({ prog: prog.name, missing: prog.pages.map((p) => p.page).filter((p) => !keyPages.includes(p)) }).toEqual({
      prog: prog.name,
      missing: [],
    });
    expect(new Set(keyPages).size).toBe(keyPages.length);
    expect(keyPages.length).toBeLessThanOrEqual(PAGE_KEY_SLOTS);
  }
});
