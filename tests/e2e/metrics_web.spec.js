import { expect, test } from "@playwright/test";
import { readFile, mkdtemp, rm } from "node:fs/promises";
import { spawn } from "node:child_process";
import path from "node:path";
import { createInterface } from "node:readline";
import { instrumentJourney, recordJourney } from "./journeys";

const scratch = process.env.TMPDIR || process.env.SP5_TEST_TMPDIR;
async function backend(mode = "ok", existing) {
  const directory = existing || (await mkdtemp(path.join(scratch, "us4-browser-")));
  const bin = process.env.SP5_MANAGEMENT_BIN;
  if (!bin || !scratch) throw new Error("Set SP5_MANAGEMENT_BIN and TMPDIR for the fixture.");
  const child = spawn(bin, [directory, path.resolve("webapp"), mode], {
    stdio: ["ignore", "pipe", "pipe"],
  });
  let errors = "";
  child.stderr.on("data", (data) => {
    errors += data;
  });
  const url = await new Promise((resolve, reject) => {
    const lines = createInterface({ input: child.stdout });
    child.once("error", reject);
    child.once("exit", (code) => reject(new Error(`C fixture exited ${code}: ${errors}`)));
    lines.once("line", (line) => {
      lines.close();
      resolve(line);
    });
  });
  return {
    url,
    directory,
    async close(remove = true) {
      if (child.exitCode === null) child.kill("SIGTERM");
      await new Promise((resolve) => {
        if (child.exitCode !== null) resolve();
        else child.once("exit", resolve);
      });
      expect(child.exitCode).toBe(0);
      expect(errors).toBe("");
      if (remove) await rm(directory, { recursive: true, force: true });
    },
  };
}

test.use({ viewport: { width: 360, height: 800 } });

// Isolated module tests run real browser DOM and controlled invalid snapshots;
// the integrated tests below use the actual C management fixture and engine.
async function modulePage(page, snapshot) {
  await page.route("http://metrics.test/**", async (route) => {
    const name = new URL(route.request().url()).pathname.slice(1);
    if (name === "metrics.js") {
      await route.fulfill({
        contentType: "text/javascript",
        body: await readFile("webapp/metrics.js", "utf8"),
      });
    } else {
      await route.fulfill({
        contentType: "text/html",
        body: '<main id="status"></main><section id="metrics"></section>',
      });
    }
  });
  await page.goto("http://metrics.test/");
  await page.evaluate(async (data) => {
    const module = await import("/metrics.js");
    window.calls = 0;
    window.pollStarts = [];
    window.activePolls = 0;
    window.maximumPolls = 0;
    window.metricsMount = await module.mount({
      statusRoot: document.querySelector("#status"),
      api: async (_path, method) => {
        window.calls += 1;
        if (method === "PATCH") return { settings: { metrics_enabled: true } };
        window.pollStarts.push(performance.now());
        window.activePolls += 1;
        window.maximumPolls = Math.max(window.maximumPolls, window.activePolls);
        if (window.responseDelay)
          await new Promise((resolve) => setTimeout(resolve, window.responseDelay));
        window.activePolls -= 1;
        return data;
      },
      refresh: async () => {},
    });
  }, snapshot);
}
function snapshot() {
  const q = (value) => ({ value_us: value, lower_bound_us: null, validity: "valid", reason: null });
  return {
    requested: false,
    effective: false,
    pending: false,
    capture: {
      session_id: "42",
      metrics_enabled: false,
      coverage: {
        state: "partial",
        enabled_intervals: [],
        gaps: [{ start: 0, end: 20 }],
        reason: "metrics_disabled",
      },
      context: {
        fps_numerator: 60000,
        fps_denominator: 1001,
        requested_bitrate_kbps: 25000,
        chosen_bitrate_kbps: 20000,
        bitrate_adjustment: "lowered_to_maximum",
      },
      summaries: ["whole_session", "last_10_seconds"].flatMap((window) =>
        ["capture", "encode", "packetize", "send", "full_pipeline", "input_delivery"].map(
          (channel, index) => ({
            channel,
            window,
            median: q(100),
            p95: q(200),
            p99: q([1000, 2000, 2000, 1000, 17000, 500][index]),
            population: {
              seen: 10,
              represented: 10,
              missing: 0,
              overflow: 0,
              failed_deliveries: 0,
            },
          }),
        ),
      ),
      retained: {
        frames: { first_us: 1, last_us: 10, records: 10 },
        inputs: { first_us: null, last_us: null, records: 0 },
      },
      continuous: {
        frame_outcomes: [10, 10, 10, 10, 0],
        encoded_bitrate_bps: 19000000,
        network_throughput_bps: 21000000,
        frame_rate_millihz: 59940,
        client_lost_packets: 0,
        client_recovery_requests: 0,
      },
    },
  };
}
test("default off; exact rational budget, stage ties and separate total/input", async ({
  page,
}) => {
  await page.setViewportSize({ width: 360, height: 800 });
  await modulePage(page, snapshot());
  await expect(page.getByRole("switch", { name: "Detailed metrics" })).not.toBeChecked();
  await expect(page.locator('[data-largest="true"]')).toHaveCount(4);
  await expect(page.getByText("Full pipeline exceeds frame budget", { exact: false })).toHaveCount(
    2,
  );
  await expect(
    page.getByText("Full pipeline exceeds frame budget", { exact: false }).first(),
  ).toBeVisible();
  await expect(page.getByText("60000/1001 fps", { exact: false })).toBeVisible();
  await expect(page.getByText("lowered_to_maximum", { exact: false })).toBeVisible();
  expect(await page.evaluate(() => document.documentElement.scrollWidth <= window.innerWidth)).toBe(
    true,
  );
});
test("overflow is unavailable with lower bound; missing channels never become zero", async ({
  page,
}) => {
  const value = snapshot();
  for (const summary of value.capture.summaries) {
    if (summary.channel === "capture")
      summary.p99 = {
        value_us: null,
        lower_bound_us: 65536,
        validity: "invalid",
        reason: "overflow",
      };
    if (summary.channel === "full_pipeline")
      summary.p99 = {
        value_us: null,
        lower_bound_us: null,
        validity: "unavailable",
        reason: "stage_unavailable",
      };
  }
  await modulePage(page, value);
  await expect(page.getByText("overflow", { exact: false }).first()).toBeVisible();
  await expect(page.getByText("65536", { exact: false }).first()).toBeVisible();
  await expect(page.getByText("stage_unavailable", { exact: false }).first()).toBeVisible();
  await expect(page.getByText("Full pipeline exceeds frame budget", { exact: false })).toHaveCount(
    0,
  );
  for (const summary of value.capture.summaries) {
    if (summary.channel === "full_pipeline")
      summary.p99 = {
        value_us: null,
        lower_bound_us: 65536,
        validity: "unavailable",
        reason: "overflow",
      };
  }
  await modulePage(page, value);
  await expect(
    page.getByText("Full pipeline lower bound exceeds frame budget", { exact: false }),
  ).toHaveCount(2);
});
test("polls do not overlap and closing the module stops browser work", async ({ page }) => {
  await modulePage(page, snapshot());
  await page.waitForTimeout(1200);
  const before = await page.evaluate(() => window.calls);
  expect(before).toBeGreaterThanOrEqual(2);
  await page.evaluate(() => window.metricsMount.dispose());
  await page.waitForTimeout(1200);
  expect(await page.evaluate(() => window.calls)).toBe(before);
});
test("visible polling keeps a one-second cadence with delayed responses", async ({ page }) => {
  await page.clock.install();
  await modulePage(page, snapshot());
  // Pause relative to the page's own fake clock: Node's wall clock can lag it.
  const pageNow = await page.evaluate(() => Date.now());
  await page.clock.pauseAt(pageNow + 1000);
  await page.evaluate(() => {
    window.responseDelay = 250;
    window.pollStarts = [];
  });
  await page.clock.runFor(3500);
  const observed = await page.evaluate(() => ({
    starts: window.pollStarts,
    maximum: window.maximumPolls,
  }));
  expect(observed.starts).toHaveLength(3);
  expect(observed.maximum).toBe(1);
  expect(observed.starts[1] - observed.starts[0]).toBe(1000);
  expect(observed.starts[2] - observed.starts[1]).toBe(1000);
});
test.describe("real C management backend", () => {
  let server;
  test.beforeEach(async () => {
    server = await backend();
  });
  test.afterEach(async () => {
    await server.close();
  });
  test("one-tap metrics persists, becomes effective in two seconds and preserves session", async ({
    page,
  }) => {
    await page.goto(server.url);
    const control = page.getByRole("switch", { name: "Detailed metrics" });
    await expect(control).toBeVisible();
    await control.setChecked(true);
    await expect(control).toBeChecked({ timeout: 2000 });
    await expect(page.getByText("Collection enabled", { exact: false })).toBeVisible({
      timeout: 2000,
    });
    const session = await page.locator("#metrics-view").getAttribute("data-session-id");
    await page.reload();
    await expect(control).toBeChecked();
    expect(await page.locator("#metrics-view").getAttribute("data-session-id")).toBe(session);
    await control.setChecked(false);
    await expect(page.getByText("Collection disabled", { exact: false })).toBeVisible({
      timeout: 2000,
    });
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(
      true,
    );
    await page
      .locator("#metrics-view article")
      .first()
      .screenshot({ path: path.join(scratch, "us4-metrics-phone.png") });
  });
  test("metrics journey: one counted tap at 360 px without scrolling or zoom", async ({
    page,
    browserName,
  }) => {
    const journey = await instrumentJourney(page);
    await page.goto(server.url);
    const control = page.getByRole("switch", { name: "Detailed metrics" });
    await expect(control).toBeVisible();
    await journey.step("home");
    await control.click();
    await expect(control).toBeChecked({ timeout: 2000 });
    await expect(page.getByText("Collection enabled", { exact: false })).toBeVisible({
      timeout: 2000,
    });
    await journey.step("metrics enabled");
    const taps = await journey.taps();
    expect(taps).toEqual(["Detailed metrics"]);
    await recordJourney("metrics", {
      browser: browserName,
      viewport: page.viewportSize(),
      taps,
      maximum: 1,
      steps: journey.steps,
    });
  });
  test("capture and redacted diagnostics downloads are complete files", async ({ page }) => {
    await page.goto(server.url);
    await page.getByRole("switch", { name: "Detailed metrics" }).setChecked(true);
    await expect(page.getByText("Collection enabled", { exact: false })).toBeVisible();
    const capture = page.waitForEvent("download");
    await page.getByRole("button", { name: "Download metrics capture" }).click();
    expect(await (await capture).failure()).toBeNull();
    const diagnostics = page.waitForEvent("download");
    await page.getByRole("button", { name: "Download diagnostics" }).click();
    expect(await (await diagnostics).failure()).toBeNull();
  });

  test("persistent requested state survives a host restart", async ({ page }) => {
    await page.goto(server.url);
    await page.getByRole("switch", { name: "Detailed metrics" }).setChecked(true);
    await expect(page.getByText("Collection enabled", { exact: false })).toBeVisible();
    const directory = server.directory;
    await server.close(false);
    server = await backend("ok", directory);
    await page.goto(server.url);
    await expect(page.getByRole("switch", { name: "Detailed metrics" })).toBeChecked({
      timeout: 2000,
    });
    await expect(page.getByText("Collection enabled", { exact: false })).toBeVisible({
      timeout: 2000,
    });
  });
  test("truncated transfer produces an error and no saved partial file", async ({ page }) => {
    await server.close();
    server = await backend("truncate");
    await page.goto(server.url);
    await page.getByRole("switch", { name: "Detailed metrics" }).setChecked(true);
    await expect(page.getByText("Collection enabled", { exact: false })).toBeVisible();
    await page.waitForTimeout(200);
    let downloads = 0;
    page.on("download", () => {
      downloads += 1;
    });
    await page.getByRole("button", { name: "Download metrics capture" }).click();
    await expect(page.locator("#feedback")).toContainText("incomplete");
    expect(downloads).toBe(0);
  });
  test("real missing and overflowing stage populations stay explicit", async ({ page }) => {
    await server.close();
    server = await backend("overflow");
    await page.goto(server.url);
    await page.getByRole("switch", { name: "Detailed metrics" }).setChecked(true);
    await expect(page.getByText("overflow", { exact: false }).first()).toBeVisible({
      timeout: 3000,
    });
    await expect(page.getByText("65536", { exact: false }).first()).toBeVisible();
    await server.close();
    server = await backend("missing");
    await page.goto(server.url);
    await page.getByRole("switch", { name: "Detailed metrics" }).setChecked(true);
    await expect(page.getByText("stage_unavailable", { exact: false }).first()).toBeVisible({
      timeout: 3000,
    });
  });
  test("hidden pages stop polling and collection continues without the page", async ({ page }) => {
    await page.goto(server.url);
    await page.getByRole("switch", { name: "Detailed metrics" }).setChecked(true);
    await expect(page.getByText("Collection enabled", { exact: false })).toBeVisible();
    await page.evaluate(() => {
      window.metricReads = 0;
      const original = window.fetch.bind(window);
      window.fetch = (...args) => {
        if (String(args[0]).endsWith("/metrics")) window.metricReads += 1;
        return original(...args);
      };
      Object.defineProperty(document, "hidden", { configurable: true, value: true });
      document.dispatchEvent(new Event("visibilitychange"));
    });
    await page.waitForTimeout(300);
    const before = await page.evaluate(() => window.metricReads);
    await page.waitForTimeout(1300);
    expect(await page.evaluate(() => window.metricReads)).toBe(before);
    const first = await page.request.get(`${server.url}/api/v1/metrics`);
    const a = await first.json();
    await page.waitForTimeout(250);
    const second = await page.request.get(`${server.url}/api/v1/metrics`);
    const b = await second.json();
    expect(b.capture.continuous.frame_outcomes[3]).toBeGreaterThan(
      a.capture.continuous.frame_outcomes[3],
    );
    await page.evaluate(() => {
      Object.defineProperty(document, "hidden", { configurable: true, value: false });
      document.dispatchEvent(new Event("visibilitychange"));
    });
    await expect.poll(() => page.evaluate(() => window.metricReads)).toBeGreaterThan(before);
  });
});
