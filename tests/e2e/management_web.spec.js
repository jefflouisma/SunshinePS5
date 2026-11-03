const { test, expect } = require("@playwright/test");
const { spawn } = require("node:child_process");
const { mkdtemp, rm } = require("node:fs/promises");
const path = require("node:path");
const { createInterface } = require("node:readline");
const { instrumentJourney, recordJourney } = require("./journeys");

// Real C management service + real POSIX persistence + synthetic registry/media.
// Loopback transport is explicitly a fake private-peer adapter, never PS5 evidence.
const root = path.resolve(__dirname, "../..");
const scratch = process.env.SP5_TEST_TMPDIR || process.env.TMPDIR;
async function host(fail = false) {
  if (!scratch || !process.env.SP5_MANAGEMENT_BIN) {
    throw new Error("Set TMPDIR and SP5_MANAGEMENT_BIN to task scratch and the built C fixture.");
  }
  const directory = await mkdtemp(path.join(scratch, "management-browser-"));
  const bin = process.env.SP5_MANAGEMENT_BIN;
  const child = spawn(
    bin,
    [directory, path.join(root, "webapp"), fail === true ? "fail" : fail || "ok"],
    {
      env: { ...process.env, TMPDIR: scratch },
      stdio: ["ignore", "pipe", "pipe"],
    },
  );
  let errors = "";
  child.stderr.on("data", (data) => {
    errors += data;
  });
  const url = await new Promise((resolve, reject) => {
    const lines = createInterface({ input: child.stdout });
    child.once("error", reject);
    child.once("exit", (code) => reject(new Error(`backend exited ${code}: ${errors}`)));
    lines.once("line", (line) => {
      resolve(line);
      lines.close();
    });
  });
  return {
    url,
    async close() {
      if (child.exitCode === null) child.kill("SIGTERM");
      await new Promise((resolve) => {
        if (child.exitCode !== null) resolve();
        else child.once("exit", resolve);
      });
      await rm(directory, { recursive: true, force: true });
      expect(errors).not.toMatch(/AddressSanitizer|runtime error:/);
      expect(child.exitCode).toBe(0);
    },
  };
}

test.use({ viewport: { width: 360, height: 780 } });

test("real status, two tabs, escaped rename and confirmed revoke in three actions", async ({
  page,
  context,
}) => {
  const backend = await host();
  try {
    const external = [];
    page.on("request", (request) => {
      if (!request.url().startsWith(backend.url)) external.push(request.url());
    });
    await page.goto(backend.url);
    await expect(page.locator("#status")).toContainText("1280 × 720");
    await expect(page.locator("#status")).toContainText("Requested target: 5000 kbps");
    await expect(page.locator("#status")).toContainText("Chosen target: 1000 kbps");
    await expect(page.locator("#status")).toContainText("lowered to maximum");
    await expect(page.locator("#status")).toContainText("Measured encoded bitrate: unavailable");
    await page.screenshot({
      path: path.join(scratch, "management-360.png"),
      fullPage: true,
    });
    const second = await context.newPage();
    await second.goto(backend.url);
    await page.getByRole("button", { name: "Paired devices" }).click();
    const first = page.locator("#devices article").first();
    await first.getByLabel("Device name").fill('<img src=x onerror="alert(1)">');
    await first.getByRole("button", { name: "Save name" }).click();
    await expect(page.locator("#feedback")).toContainText("saved");
    await second.getByRole("button", { name: "Paired devices" }).click();
    await expect(second.locator("#devices")).toContainText('<img src=x onerror="alert(1)">');
    expect(await page.locator("#devices img").count()).toBe(0);
    await second.locator("#devices article").first().getByLabel("Device name").fill("Unsaved edit");
    await page.getByRole("button", { name: "Home", exact: true }).click();
    await page.getByRole("button", { name: "Paired devices" }).click();
    await page.locator("#devices article").first().getByRole("button", { name: "Revoke" }).click();
    await expect(page.getByRole("dialog")).toBeVisible();
    await page.getByRole("button", { name: "Confirm revoke" }).click();
    await expect(page.locator("#devices article")).toHaveCount(1);
    await expect(second.locator("#devices article")).toHaveCount(1);
    // The revoked device's active session ended before the acknowledgement.
    await expect(page.locator("#status")).toContainText("No active session");
    expect(await page.evaluate(() => document.documentElement.scrollWidth <= innerWidth)).toBe(
      true,
    );
    expect(external).toEqual([]);
  } finally {
    await backend.close();
  }
});

test("durability errors preserve rename/settings and revoke still denies access", async ({
  page,
}) => {
  const backend = await host(true);
  try {
    await page.goto(backend.url);
    await page.getByRole("button", { name: "Host settings" }).click();
    await page.getByLabel("Host name").fill("Unsaved");
    await page.getByRole("button", { name: "Save host name" }).click();
    await expect(page.locator("#feedback")).toContainText("persist");
    await page.getByRole("button", { name: "Home", exact: true }).click();
    await page.getByRole("button", { name: "Paired devices" }).click();
    await page.locator("#devices article").first().getByRole("button", { name: "Revoke" }).click();
    await page.getByRole("button", { name: "Confirm revoke" }).click();
    await expect(page.locator("#feedback")).toContainText("restart");
    await expect(page.locator("#devices article")).toHaveCount(1);
    await expect(page.locator("#status")).toContainText("No active session");
  } finally {
    await backend.close();
  }
});

test("an unsaved revocation stays visible until a retried save succeeds", async ({ page }) => {
  const backend = await host("fail-once");
  try {
    await page.goto(backend.url);
    await page.getByRole("button", { name: "Paired devices" }).click();
    await expect(page.locator("#devices article")).toHaveCount(2);
    await page.locator("#devices article").first().getByRole("button", { name: "Revoke" }).click();
    await page.getByRole("button", { name: "Confirm revoke" }).click();
    await expect(page.locator("#feedback")).toContainText("Restart protection is not confirmed");
    await expect(page.locator("#devices article")).toHaveCount(1);
    const warning = page.locator("#devices .warning");
    await expect(warning).toContainText("A revocation is not saved");
    await expect(warning).toContainText("Tablet");
    await expect(page.locator("#status")).toContainText("A revocation is not saved");
    await page.getByRole("button", { name: "Home", exact: true }).click();
    await page.getByRole("button", { name: "Paired devices" }).click();
    await expect(warning).toBeVisible();
    await warning.getByRole("button", { name: "Retry save" }).click();
    await expect(page.locator("#feedback")).toContainText("Revocation saved");
    await expect(page.locator("#devices .warning")).toHaveCount(0);
    await expect(page.locator("#status")).not.toContainText("A revocation is not saved");
    await expect(page.locator("#devices article")).toHaveCount(1);
  } finally {
    await backend.close();
  }
});

test("unreadable saved settings fall back to defaults with a visible next step", async ({
  page,
}) => {
  const backend = await host("recovered-settings");
  try {
    await page.goto(backend.url);
    await expect(page.locator("#status .warning")).toContainText(
      "Saved settings were unreadable; defaults are in use.",
    );
    await expect(page.locator("#status")).toContainText("SunshinePS5");
    // The store's load diagnostic also reaches the downloadable diagnostics bundle.
    const bundle = await page.evaluate(async () => (await fetch("/api/v1/diagnostics")).text());
    expect(bundle).toContain('"event":"diagnostic","reason":"invalid_argument"');
    await page.getByRole("button", { name: "Host settings" }).click();
    await page.getByLabel("Host name").fill("Den");
    await page.getByRole("button", { name: "Save host name" }).click();
    await expect(page.locator("#feedback")).toContainText("Host name saved");
    await expect(page.locator("#status .warning")).toHaveCount(0);
  } finally {
    await backend.close();
  }
});

test("durable revocation cleanup remains retryable after reload with confirmation", async ({
  page,
}) => {
  const backend = await host("cleanup-fail-once");
  try {
    const journey = await instrumentJourney(page);
    await page.goto(backend.url);
    await page.getByRole("button", { name: "Paired devices" }).click();
    const devices = await page.evaluate(async () => (await fetch("/api/v1/devices")).json());
    const selected = devices.devices[0];
    await page.locator("#devices article").first().getByRole("button", { name: "Revoke" }).click();
    await page.getByRole("button", { name: "Confirm revoke" }).click();
    await expect(page.locator("#feedback")).toContainText("Removal persisted");
    await expect(page.locator("#devices article")).toHaveCount(1);
    const pending = await page.evaluate(async () => (await fetch("/api/v1/devices")).json());
    expect(pending.revocation_durability_pending).toBe(false);
    expect(pending.pending_revocations).toEqual([]);
    expect(pending.cleanup_pending_revocations).toEqual([{ id: selected.id, name: selected.name }]);
    await page.reload();
    await page.getByRole("button", { name: "Paired devices" }).click();
    const warning = page.locator("#devices .warning");
    await expect(warning).toContainText("Session cleanup is incomplete");
    await expect(warning).toContainText(selected.name);
    expect(await page.locator("#devices img").count()).toBe(0);
    await page.screenshot({
      path: path.join(scratch, "revocation-cleanup-pending.png"),
      fullPage: true,
    });
    await warning.getByRole("button", { name: "Retry cleanup" }).click();
    await expect(page.getByRole("dialog")).toContainText(selected.name);
    expect(await page.getByRole("dialog").locator("img").count()).toBe(0);
    await page.getByRole("button", { name: "Cancel" }).click();
    await expect(warning).toBeVisible();
    // Reload again: the completed recovery is three taps from home.
    await page.reload();
    await journey.step("pending cleanup home");
    await page.getByRole("button", { name: "Paired devices" }).click();
    await journey.step("pending cleanup devices");
    await warning.getByRole("button", { name: "Retry cleanup" }).click();
    await journey.step("pending cleanup confirmation");
    await page.getByRole("button", { name: "Confirm retry" }).click();
    await expect(page.locator("#feedback")).toContainText("Session cleanup completed");
    await expect(warning).toHaveCount(0);
    await expect(page.locator("#devices article")).toHaveCount(1);
    await expect(page.locator("#status")).toContainText("No active session");
    const complete = await page.evaluate(async () => (await fetch("/api/v1/devices")).json());
    expect(complete.cleanup_pending_revocations).toEqual([]);
    expect(complete.devices.map((device) => device.name)).toEqual(["Phone"]);
    expect(await api(page, "DELETE", `/devices/${selected.id}`, { confirm: true })).toBe(404);
    await journey.step("cleanup completed");
    expect(await journey.taps()).toEqual(["devices-button", "Retry cleanup", "confirm-action"]);
    await page.screenshot({
      path: path.join(scratch, "revocation-cleanup-complete.png"),
      fullPage: true,
    });
    await recordJourney("revocation-cleanup-retry", {
      taps: await journey.taps(),
      maximum: 3,
      steps: journey.steps,
    });
  } finally {
    await backend.close();
  }
});

test("stop requires confirmation, acknowledges stopping and shows unavailable after exit", async ({
  page,
}) => {
  const backend = await host();
  try {
    await page.goto(backend.url);
    await expect(page.locator("#status")).toContainText("running");
    await page.getByRole("button", { name: "Stop host", exact: true }).click();
    await page.getByRole("button", { name: "Cancel" }).click();
    await expect(page.locator("#status")).toContainText("running");
    await page.getByRole("button", { name: "Stop host", exact: true }).click();
    await page.getByRole("button", { name: "Confirm stop" }).click();
    await expect(page.locator("#feedback")).toContainText("Stopping");
    await expect(page.locator("#status")).toContainText("Unavailable");
    await expect(page.locator("body")).not.toContainText("Host stopped successfully");
  } finally {
    await backend.close();
  }
});

test("stale tokens, hostile JSON and visible-only nonoverlapping polling", async ({ page }) => {
  const backend = await host();
  try {
    await page.goto(backend.url);
    const stale = await page.evaluate(async () => {
      const response = await fetch("/api/v1/settings", {
        method: "PATCH",
        headers: { "Content-Type": "application/json", "X-SP5-Token": "stale" },
        body: '{"host_name":"Bad"}',
      });
      return { status: response.status, body: await response.json() };
    });
    expect(stale.status).toBe(403);
    expect(stale.body.error.recovery).toBeTruthy();
    expect(stale.body.error.request_id).toBeTruthy();
    await page.evaluate(() => {
      window.fetchActive = 0;
      window.fetchMaximum = 0;
      window.fetchCount = 0;
      const original = window.fetch.bind(window);
      window.fetch = async (...args) => {
        window.fetchActive++;
        window.fetchCount++;
        window.fetchMaximum = Math.max(window.fetchMaximum, window.fetchActive);
        try {
          const response = await original(...args);
          await new Promise((resolve) => setTimeout(resolve, 350));
          return response;
        } finally {
          window.fetchActive--;
        }
      };
    });
    await page.waitForTimeout(2400);
    expect(await page.evaluate(() => window.fetchMaximum)).toBe(1);
    await page.evaluate(() => {
      Object.defineProperty(document, "hidden", { configurable: true, value: true });
      document.dispatchEvent(new Event("visibilitychange"));
    });
    await page.waitForTimeout(800);
    const count = await page.evaluate(() => window.fetchCount);
    await page.waitForTimeout(1300);
    expect(await page.evaluate(() => window.fetchCount)).toBe(count);
    await page.evaluate(() => {
      Object.defineProperty(document, "hidden", { configurable: true, value: false });
      document.dispatchEvent(new Event("visibilitychange"));
    });
    await expect.poll(() => page.evaluate(() => window.fetchCount)).toBeGreaterThan(count);
  } finally {
    await backend.close();
  }
});

test("home PIN submission uses a real pending request and reports unavailable proof honestly", async ({
  page,
}) => {
  const backend = await host("pairing");
  try {
    await page.goto(backend.url);
    await expect(page.locator("#pin-form")).toBeVisible();
    await page.getByLabel("Four-digit PIN").click();
    await page.getByLabel("Four-digit PIN").fill("1234");
    await page.getByRole("button", { name: "Submit PIN" }).click();
    await expect(page.locator("#feedback")).toContainText("proof service unavailable");
    await expect(page.locator("#feedback")).toBeFocused();
    const stale = await page.evaluate(async () => {
      const token = await (await fetch("/api/v1/page")).json();
      return (
        await fetch("/api/v1/pairing/pin", {
          method: "POST",
          headers: { "Content-Type": "application/json", "X-SP5-Token": token.token },
          body: JSON.stringify({ request_id: 999, pin: "1234" }),
        })
      ).status;
    });
    expect(stale).toBe(409);
  } finally {
    await backend.close();
  }
});

test("home PIN accepts a real protocol handshake in two actions without granting authorization", async ({
  page,
}) => {
  const backend = await host("pairing-ready");
  try {
    await page.goto(backend.url);
    await expect(page.locator("#pin-form")).toBeVisible();
    await page.getByLabel("Four-digit PIN").click();
    await page.getByLabel("Four-digit PIN").fill("1234");
    await page.getByRole("button", { name: "Submit PIN" }).click();
    // FR-039 plain language: the raw proof_pending state is never shown.
    await expect(page.locator("#feedback")).toContainText("PIN submitted");
    await expect(page.locator("#feedback")).toContainText("Finish pairing in Moonlight");
    await expect(page.locator("#pairing-state")).toContainText("PIN sent for SyntheticPhone");
    await expect(page.locator("#pin-form")).toBeHidden();
    await expect(page.locator("#home")).not.toContainText("proof_pending");
    const devices = await page.evaluate(
      async () => (await (await fetch("/api/v1/devices")).json()).devices,
    );
    expect(devices).toHaveLength(2);
  } finally {
    await backend.close();
  }
});

async function api(page, method, path, body) {
  return page.evaluate(
    async ({ method, path, body }) => {
      const token = (await (await fetch("/api/v1/page")).json()).token;
      const response = await fetch(`/api/v1${path}`, {
        method,
        headers: { "Content-Type": "application/json", "X-SP5-Token": token },
        body: JSON.stringify(body),
      });
      return response.status;
    },
    { method, path, body },
  );
}

test("valid 63-character unbroken names never scroll horizontally at 360 px", async ({ page }) => {
  const backend = await host();
  try {
    await page.goto(backend.url);
    const long = "W".repeat(63);
    expect(await api(page, "PATCH", "/settings", { host_name: long })).toBe(200);
    const devices = await page.evaluate(
      async () => (await (await fetch("/api/v1/devices")).json()).devices,
    );
    for (const [index, device] of devices.entries())
      expect(
        await api(page, "PATCH", `/devices/${device.id}`, { name: long.slice(1) + index }),
      ).toBe(200);
    const width = () =>
      page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
    await page.reload();
    await expect(page.locator("#status")).toContainText(long);
    expect(await width()).toBeLessThanOrEqual(0);
    await page.getByRole("button", { name: "Paired devices" }).click();
    await expect(page.locator("#devices article")).toHaveCount(2);
    expect(await width()).toBeLessThanOrEqual(0);
    await page.locator("#devices article").first().getByRole("button", { name: "Revoke" }).click();
    await expect(page.getByRole("dialog")).toBeVisible();
    expect(await width()).toBeLessThanOrEqual(0);
    await page.getByRole("button", { name: "Cancel" }).click();
    await page.getByRole("button", { name: "Host settings" }).click();
    expect(await width()).toBeLessThanOrEqual(0);
  } finally {
    await backend.close();
  }
});

test("twenty reloads keep management usable with one page token per load", async ({
  page,
  context,
}) => {
  const backend = await host();
  try {
    const tokens = [];
    page.on("request", (request) => {
      if (request.url().endsWith("/api/v1/page")) tokens.push(request.url());
    });
    for (let load = 0; load < 20; load++) {
      const issued = page.waitForResponse((response) => response.url().endsWith("/api/v1/page"));
      if (load === 0) await page.goto(backend.url);
      else await page.reload();
      expect((await issued).status()).toBe(200);
      await expect(page.locator("#status")).toContainText("running");
      await expect(page.locator("#pairing-state")).toContainText("Start pairing in Moonlight");
      await page.waitForTimeout(150);
      expect(tokens).toHaveLength(load + 1);
    }
    await page.getByRole("button", { name: "Host settings" }).click();
    await page.getByLabel("Host name").fill("After reloads");
    await page.getByRole("button", { name: "Save host name" }).click();
    await expect(page.locator("#feedback")).toContainText("Host name saved");
    // Another tab replaces every page token; this page renews its own once.
    const other = await context.newPage();
    await other.goto(backend.url);
    await other.evaluate(async () => {
      for (let i = 0; i < 16; i++) await fetch("/api/v1/page");
    });
    await page.getByLabel("Host name").fill("Renewed");
    await page.getByRole("button", { name: "Save host name" }).click();
    await expect(page.locator("#feedback")).toContainText("nothing was changed");
    await expect(page.locator("#status")).toContainText("After reloads");
    await page.getByLabel("Host name").fill("Renewed");
    await page.getByRole("button", { name: "Save host name" }).click();
    await expect(page.locator("#feedback")).toContainText("Host name saved");
    await expect(page.locator("#status")).toContainText("Renewed");
    expect(tokens).toHaveLength(21);
  } finally {
    await backend.close();
  }
});

test("revoke journey: three counted taps at 360 px without scrolling or zoom", async ({
  page,
  browserName,
}) => {
  const backend = await host();
  try {
    const journey = await instrumentJourney(page);
    await page.goto(backend.url);
    await expect(page.locator("#status")).toContainText("running");
    await journey.step("home");
    await page.getByRole("button", { name: "Paired devices" }).click();
    await expect(page.locator("#devices article")).toHaveCount(2);
    await journey.step("paired devices");
    await page.locator("#devices article").first().getByRole("button", { name: "Revoke" }).click();
    await expect(page.getByRole("dialog")).toBeVisible();
    await journey.step("confirmation");
    await page.getByRole("button", { name: "Confirm revoke" }).click();
    await expect(page.locator("#devices article")).toHaveCount(1);
    await expect(page.locator("#feedback")).toContainText("Device revoked");
    await journey.step("revoked");
    const taps = await journey.taps();
    expect(taps).toEqual(["devices-button", "Revoke", "confirm-action"]);
    await recordJourney("revoke", {
      browser: browserName,
      viewport: page.viewportSize(),
      taps,
      maximum: 3,
      steps: journey.steps,
    });
  } finally {
    await backend.close();
  }
});
