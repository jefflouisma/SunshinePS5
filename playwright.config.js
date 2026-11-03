const { defineConfig } = require("@playwright/test");
const path = require("node:path");
const scratch = process.env.SP5_TEST_TMPDIR || process.env.TMPDIR;
if (!scratch) throw new Error("Set TMPDIR to the browser-test scratch directory.");
module.exports = defineConfig({
  testDir: "./tests/e2e",
  testMatch: "*_web.spec.js",
  // CI must never silently narrow the discovered suites.
  forbidOnly: !!process.env.CI,
  timeout: 30000,
  workers: 1,
  reporter: "line",
  outputDir: path.join(scratch, "playwright"),
  use: { browserName: "chromium", headless: true },
});
