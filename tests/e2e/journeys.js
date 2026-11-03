// SYNTHETIC phone-journey instrumentation for fixture runs (T108 preparation).
// It counts navigation, submission and confirmation taps the way the web
// contract's journey table does, and checks horizontal scrolling and zoom after
// every step. Results come from headless engines against fake hosts: they are
// never SC-011 evidence and must not be copied into docs/evidence/.
const { expect } = require("@playwright/test");
const { mkdir, writeFile } = require("node:fs/promises");
const path = require("node:path");

// Counted controls. Taps that only focus text fields for typing are excluded,
// except the journey's designated pairing control (the PIN field).
const COUNTED = 'button, a[href], input[type="checkbox"], input[type="radio"], [role="switch"]';

async function instrumentJourney(page, { focusControls = [] } = {}) {
  await page.addInitScript(
    ({ counted, focus }) => {
      window.__journeyTaps = [];
      document.addEventListener(
        "click",
        (event) => {
          if (!event.isTrusted || !(event.target instanceof Element)) return;
          const control =
            event.target.closest(counted) ||
            focus.map((selector) => event.target.closest(selector)).find(Boolean);
          if (control)
            window.__journeyTaps.push(
              control.id || control.getAttribute("aria-label") || control.textContent.trim(),
            );
        },
        true,
      );
    },
    { counted: COUNTED, focus: focusControls },
  );
  const steps = [];
  return {
    steps,
    async taps() {
      return page.evaluate(() => [...window.__journeyTaps]);
    },
    // Asserts no horizontal scrolling, no zoom and no input small enough to
    // trigger mobile focus zoom, then records the observation.
    async step(label) {
      const layout = await page.evaluate(() => ({
        scrollWidth: document.documentElement.scrollWidth,
        innerWidth: window.innerWidth,
        scale: window.visualViewport ? window.visualViewport.scale : 1,
        smallInputs: [...document.querySelectorAll("input, select, textarea")]
          .filter((element) => element.getClientRects().length > 0)
          .filter((element) => Number.parseFloat(getComputedStyle(element).fontSize) < 16)
          .map((element) => element.id || element.getAttribute("aria-label") || element.type),
      }));
      expect(layout.scrollWidth, `${label}: horizontal scrolling`).toBeLessThanOrEqual(
        layout.innerWidth,
      );
      expect(layout.scale, `${label}: zoomed viewport`).toBe(1);
      expect(layout.smallInputs, `${label}: inputs below 16px`).toEqual([]);
      steps.push({ label, ...layout });
    },
  };
}

// Writes a scratch summary that is explicitly labeled synthetic.
async function recordJourney(name, summary) {
  const scratch = process.env.SP5_TEST_TMPDIR || process.env.TMPDIR;
  const directory = path.join(scratch, "journeys");
  await mkdir(directory, { recursive: true });
  await writeFile(
    path.join(directory, `${name}.json`),
    `${JSON.stringify(
      {
        synthetic: true,
        sc011_evidence: false,
        note: "Headless fixture run against a fake host; not a real-browser SC-011 observation.",
        journey: name,
        ...summary,
      },
      null,
      2,
    )}\n`,
  );
}

module.exports = { instrumentJourney, recordJourney };
