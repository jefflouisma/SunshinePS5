const { test, expect } = require("@playwright/test");
const { spawn } = require("node:child_process");
const { randomInt } = require("node:crypto");
const { mkdir, mkdtemp, rm, writeFile } = require("node:fs/promises");
const net = require("node:net");
const path = require("node:path");
const { createInterface } = require("node:readline");
const { instrumentJourney, recordJourney } = require("./journeys");

// T077 phone pairing journeys. Two SYNTHETIC backends, never PS5 or official
// client evidence:
// - sp5_management_host on loopback (fake private peer at its adapter) with an
//   in-process synthetic Gen7 client that proves with its own PIN;
// - sp5_fake_host on a real RFC1918 address with the independent Rust client
//   (CI; it is not run under a loopback-only local policy).
const root = path.resolve(__dirname, "../..");
const scratch = process.env.SP5_TEST_TMPDIR || process.env.TMPDIR;
const randomPin = () => String(randomInt(0, 10000)).padStart(4, "0");

async function management(mode, { directory, env = {} } = {}) {
  if (!scratch || !process.env.SP5_MANAGEMENT_BIN)
    throw new Error("Set TMPDIR and SP5_MANAGEMENT_BIN to task scratch and the built C fixture.");
  const state = directory || (await mkdtemp(path.join(scratch, "pairing-browser-")));
  const child = spawn(process.env.SP5_MANAGEMENT_BIN, [state, path.join(root, "webapp"), mode], {
    env: { ...process.env, ...env, TMPDIR: scratch },
    stdio: ["ignore", "pipe", "pipe"],
  });
  let errors = "";
  child.stderr.on("data", (data) => {
    errors += data;
  });
  const url = await new Promise((resolve, reject) => {
    const lines = createInterface({ input: child.stdout });
    child.once("error", reject);
    child.once("exit", (code) => reject(new Error(`fixture exited ${code}: ${errors}`)));
    lines.once("line", (line) => {
      lines.close();
      resolve(line);
    });
  });
  return {
    url,
    directory: state,
    async close(remove = true) {
      if (child.exitCode === null) child.kill("SIGTERM");
      await new Promise((resolve) => {
        if (child.exitCode !== null) resolve();
        else child.once("exit", resolve);
      });
      expect(errors).not.toMatch(/AddressSanitizer|runtime error:/);
      expect(child.exitCode).toBe(0);
      if (remove) await rm(state, { recursive: true, force: true });
    },
  };
}

// Exact bytes on the wire, so duplicate and forged headers stay intact.
function raw(url, request) {
  const { hostname, port } = new URL(url);
  return new Promise((resolve, reject) => {
    // The fixture answers with Connection: close; no half-close is needed.
    const socket = net.connect(Number(port), hostname, () => socket.write(request));
    let data = "";
    socket.setEncoding("utf8");
    socket.on("data", (chunk) => {
      data += chunk;
    });
    socket.on("error", reject);
    socket.on("close", () => {
      const status = Number(data.split(" ")[1]);
      resolve({ status, body: data.slice(data.indexOf("\r\n\r\n") + 4) });
    });
  });
}
function wire(method, target, headers, body = "") {
  return `${method} ${target} HTTP/1.1\r\n${headers.join("\r\n")}\r\nContent-Length: ${Buffer.byteLength(body)}\r\n\r\n${body}`;
}
async function pageToken(page) {
  return page.evaluate(async () => (await (await fetch("/api/v1/page")).json()).token);
}
async function pairingView(page) {
  return page.evaluate(async () => (await fetch("/api/v1/pairing")).json());
}
async function overflow(page) {
  return page.evaluate(() => document.documentElement.scrollWidth - window.innerWidth);
}

test.use({ viewport: { width: 360, height: 780 } });

test("correct PIN pairs within three seconds in two counted taps and survives restart", async ({
  page,
  browserName,
}) => {
  const pin = randomPin();
  let backend = await management(`pairing-proof=${pin}`);
  try {
    const external = [];
    await page.route("**/*", (route) => {
      if (route.request().url().startsWith(backend.url)) return route.continue();
      external.push(route.request().url());
      return route.abort();
    });
    const journey = await instrumentJourney(page, { focusControls: ["#pin"] });
    await page.goto(backend.url);
    await expect(page.locator("#pairing-state")).toContainText("Pair SyntheticPhone");
    await expect(page.locator("#pin-form")).toBeVisible();
    await journey.step("pending request shown");
    await page.getByLabel("Four-digit PIN").click();
    await page.getByLabel("Four-digit PIN").fill(pin);
    // Focusing the optional name field is typing, which the contract excludes.
    await page.getByLabel("Device name (optional)").click();
    await page.getByLabel("Device name (optional)").fill("Den phone");
    await journey.step("PIN typed");
    const submitted = Date.now();
    await page.getByRole("button", { name: "Submit PIN" }).click();
    await expect(page.locator("#pairing-state")).toContainText("Paired Den phone", {
      timeout: 3000,
    });
    const elapsed = Date.now() - submitted;
    expect(elapsed).toBeLessThanOrEqual(3000);
    await expect(page.locator("#feedback")).toContainText("Paired Den phone");
    await expect(page.locator("#pin-form")).toBeHidden();
    await journey.step("pairing confirmed");
    const taps = await journey.taps();
    expect(taps).toEqual(["pin", "Submit PIN"]);
    await recordJourney("pin", {
      browser: browserName,
      viewport: page.viewportSize(),
      taps,
      maximum: 2,
      completion_ms: elapsed,
      steps: journey.steps,
    });
    await page.getByRole("button", { name: "Paired devices" }).click();
    await expect(page.locator("#devices")).toContainText("Den phone");
    await expect(page.locator("#devices article")).toHaveCount(3);
    expect(external).toEqual([]);
    // Persistent paired status: a new process over the same durable state.
    const directory = backend.directory;
    await backend.close(false);
    backend = await management("ok", { directory });
    await page.goto(backend.url);
    await page.getByRole("button", { name: "Paired devices" }).click();
    await expect(page.locator("#devices article")).toHaveCount(3);
    await expect(page.locator("#devices")).toContainText("Den phone");
    expect(await overflow(page)).toBeLessThanOrEqual(0);
  } finally {
    await backend.close();
  }
});

test("wrong PIN explains the failure, remaining attempts and how to retry", async ({ page }) => {
  const pin = randomPin();
  const wrong = String((Number(pin) + 1) % 10000).padStart(4, "0");
  const backend = await management(`pairing-proof=${pin}`);
  try {
    await page.goto(backend.url);
    await page.getByLabel("Four-digit PIN").click();
    await page.getByLabel("Four-digit PIN").fill(wrong);
    await page.getByRole("button", { name: "Submit PIN" }).click();
    const feedback = page.locator("#feedback");
    await expect(feedback).toContainText("Pairing SyntheticPhone failed", { timeout: 3000 });
    await expect(feedback).toContainText("the PIN did not match");
    await expect(feedback).toContainText("Start pairing again in Moonlight");
    await expect(feedback).toContainText("4 attempts remain before a 60-second pause");
    await expect(feedback).toBeFocused();
    await expect(page.locator("#pin-form")).toBeHidden();
    const view = await pairingView(page);
    expect(view.pending).toBeNull();
    expect(view.last).toMatchObject({ state: "failed", failures: 1, attempts_before_cooldown: 4 });
    const devices = await page.evaluate(
      async () => (await (await fetch("/api/v1/devices")).json()).devices,
    );
    expect(devices).toHaveLength(2);
    for (const selector of ["#pairing", "#feedback"])
      expect(await page.locator(selector).textContent()).not.toMatch(/proof_pending|undefined/);
  } finally {
    await backend.close();
  }
});

test("hostile 256-byte client names stay inert text and fit 360 pixels", async ({ page }) => {
  const hostile = '<img src=x onerror="window.pwned=1">';
  const name = hostile + "W".repeat(256 - hostile.length);
  const backend = await management(`pairing-proof=${randomPin()}`, {
    env: { SP5_SYNTHETIC_CLIENT_NAME: name },
  });
  try {
    await page.goto(backend.url);
    await expect(page.locator("#pairing-state")).toContainText(`Pair ${name}`);
    expect(await page.locator("#pairing img").count()).toBe(0);
    expect(await page.evaluate(() => window.pwned)).toBeUndefined();
    const view = await pairingView(page);
    expect(view.pending.client_name).toBe(name);
    expect(await overflow(page)).toBeLessThanOrEqual(0);
    await page.getByLabel("Four-digit PIN").click();
    expect(await overflow(page)).toBeLessThanOrEqual(0);
  } finally {
    await backend.close();
  }
});

test("a form shown for one request is never retargeted to the next", async ({ page }) => {
  const backend = await management("pairing-ready");
  try {
    let replacement;
    const posted = [];
    await page.route("**/api/v1/pairing", async (route) => {
      const response = await route.fetch();
      const value = await response.json();
      // Controlled response: simulate request A ending and request B arriving.
      if (replacement && value.pending)
        value.pending = { ...value.pending, id: replacement, client_name: "Other phone" };
      await route.fulfill({ response, json: value });
    });
    page.on("request", (request) => {
      if (request.url().endsWith("/api/v1/pairing/pin")) posted.push(request.postDataJSON());
    });
    await page.goto(backend.url);
    await expect(page.locator("#pin-form")).toBeVisible();
    const original = (await pairingView(page)).pending.id;
    expect(original).toMatch(/^[1-9][0-9]*$/);
    await page.getByLabel("Four-digit PIN").fill("1234");
    replacement = (BigInt(original) + 1n).toString();
    await expect(page.locator("#feedback")).toContainText("replaced the previous one");
    await expect(page.getByLabel("Four-digit PIN")).toHaveValue("");
    await expect(page.locator("#pairing-state")).toContainText("Pair Other phone");
    await page.getByLabel("Four-digit PIN").fill("5678");
    await page.getByRole("button", { name: "Submit PIN" }).click();
    await expect(page.locator("#feedback")).toContainText("no longer pending");
    expect(posted).toEqual([{ request_id: replacement, pin: "5678" }]);
    // The real host still holds request A, untouched by either form.
    // Wait for an in-flight background poll handler before falling back to the host.
    await page.unrouteAll({ behavior: "wait" });
    const view = await pairingView(page);
    expect(view.pending).toMatchObject({ id: original, pin_entered: false });
  } finally {
    await backend.close();
  }
});

test("a PIN typed before its request ended is cleared for the next request", async ({ page }) => {
  const backend = await management("pairing-ready");
  try {
    let phase = "real";
    let original;
    const posted = [];
    await page.route("**/api/v1/pairing", async (route) => {
      const response = await route.fetch();
      const value = await response.json();
      // Controlled responses: request A expires, then request B arrives.
      if (phase === "ended")
        Object.assign(value, {
          pending: null,
          last: {
            id: original,
            state: "expired",
            client_name: "SyntheticPhone",
            failures: 0,
            attempts_before_cooldown: 5,
          },
        });
      if (phase === "next")
        value.pending = { ...value.pending, id: (BigInt(original) + 1n).toString() };
      await route.fulfill({ response, json: value });
    });
    page.on("request", (request) => {
      if (request.url().endsWith("/api/v1/pairing/pin")) posted.push(request.postDataJSON());
    });
    await page.goto(backend.url);
    await expect(page.locator("#pin-form")).toBeVisible();
    original = (await pairingView(page)).pending.id;
    await page.getByLabel("Four-digit PIN").fill("1234");
    phase = "ended";
    await expect(page.locator("#pairing-state")).toContainText("expired");
    await expect(page.locator("#pin-form")).toBeHidden();
    phase = "next";
    await expect(page.locator("#pin-form")).toBeVisible();
    await expect(page.getByLabel("Four-digit PIN")).toHaveValue("");
    expect(posted).toEqual([]);
  } finally {
    await backend.close();
  }
});

test("pairing routes enforce Host, Origin, token and content-type boundaries", async ({
  page,
  context,
}) => {
  const backend = await management("pairing-ready");
  try {
    await page.goto(backend.url);
    const host = new URL(backend.url).host;
    const { id } = (await pairingView(page)).pending;
    const token = await pageToken(page);
    const body = JSON.stringify({ request_id: id, pin: "1234" });
    const base = [`Host: ${host}`, `Origin: http://${host}`];
    const denied = [
      ["Host: evil.test"],
      [`Host: ${host}`, `Host: ${host}`],
      [`Host: ${host}`, "Origin: https://evil.test"],
      [`Host: ${host}`, "Origin: null"],
      [`Host: ${host}`, "Sec-Fetch-Site: cross-site"],
    ];
    for (const headers of denied) {
      const read = await raw(backend.url, wire("GET", "/api/v1/pairing", headers));
      expect(read.status).toBe(403);
      expect(read.body).not.toContain("client_name");
      const write = await raw(
        backend.url,
        wire(
          "POST",
          "/api/v1/pairing/pin",
          [...headers, "Content-Type: application/json", `X-SP5-Token: ${token}`],
          body,
        ),
      );
      expect(write.status).toBe(403);
    }
    const json = "Content-Type: application/json";
    for (const headers of [[json], [json, "X-SP5-Token: stale"]]) {
      const write = await raw(
        backend.url,
        wire("POST", "/api/v1/pairing/pin", [...base, ...headers], body),
      );
      expect(write.status).toBe(403);
      expect(JSON.parse(write.body).error.code).toBe("stale_token");
    }
    const plain = await raw(
      backend.url,
      wire(
        "POST",
        "/api/v1/pairing/pin",
        [...base, "Content-Type: text/plain", `X-SP5-Token: ${token}`],
        body,
      ),
    );
    expect(plain.status).toBe(400);
    // A rebinding name for the same socket is refused before any app byte.
    const port = new URL(backend.url).port;
    const alias = await context.newPage();
    const response = await alias.goto(`http://localhost:${port}/`);
    expect(response.status()).toBe(403);
    expect(await alias.locator("#pin-form").count()).toBe(0);
    // Page tokens are not issued to navigations or embedded loads.
    const navigation = await alias.goto(`${backend.url}/api/v1/page`);
    expect(navigation.status()).toBe(403);
    expect(await navigation.text()).not.toContain('"token"');
    const view = await pairingView(page);
    expect(view.pending).toMatchObject({ id, pin_entered: false });
  } finally {
    await backend.close();
  }
});

// Real-protocol journey: browser PIN, independent Rust client, private fake host.
const fakeHostEnv = ["SP5_FAKE_HOST_BIN", "SP5_CLIENT_BIN", "SP5_PRIVATE_BIND"];
function privateBind(value) {
  const parts = value.split(".").map(Number);
  const valid =
    parts.length === 4 &&
    parts.every((part) => Number.isInteger(part) && part >= 0 && part <= 255) &&
    (parts[0] === 10 ||
      (parts[0] === 172 && parts[1] >= 16 && parts[1] <= 31) ||
      (parts[0] === 192 && parts[1] === 168));
  if (!valid) throw new Error("SP5_PRIVATE_BIND must be an actual RFC1918 IPv4 address");
  return value;
}
async function fakeHost(directory) {
  const bind = privateBind(process.env.SP5_PRIVATE_BIND);
  const child = spawn(
    process.env.SP5_FAKE_HOST_BIN,
    [
      bind,
      path.join(directory, "state"),
      path.join(directory, "video.h264"),
      path.join(directory, "audio.opus"),
      path.join(directory, "metrics.ndjson"),
      "0",
    ],
    { env: { ...process.env, TMPDIR: scratch }, stdio: ["pipe", "pipe", "pipe"] },
  );
  let errors = "";
  child.stderr.on("data", (data) => {
    errors += data;
  });
  const events = [];
  const waiting = new Set();
  createInterface({ input: child.stdout }).on("line", (line) => {
    events.push(JSON.parse(line));
    for (const check of waiting) check();
  });
  let cursor = 0;
  function event(name, timeout = 15000) {
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        waiting.delete(check);
        reject(new Error(`missing host event ${name}: ${errors}`));
      }, timeout);
      function check() {
        const index = events.findIndex((item, at) => at >= cursor && item.event === name);
        if (index < 0) return;
        cursor = index + 1;
        clearTimeout(timer);
        waiting.delete(check);
        resolve(events[index]);
      }
      waiting.add(check);
      check();
    });
  }
  child.once("exit", () => {
    for (const check of waiting) check();
  });
  const { url } = await event("web");
  await event("ready");
  return {
    url,
    bind,
    event,
    command(line) {
      child.stdin.write(`${line}\n`);
    },
    async close() {
      if (child.exitCode === null) child.stdin.end("stop\n");
      await new Promise((resolve) => {
        if (child.exitCode !== null) resolve();
        else child.once("exit", resolve);
      });
      expect(errors).toBe("");
      expect(child.exitCode).toBe(0);
    },
  };
}
function client(args) {
  const child = spawn(process.env.SP5_CLIENT_BIN, args, { stdio: ["ignore", "pipe", "pipe"] });
  let output = "";
  child.stdout.on("data", (data) => {
    output += data;
  });
  child.stderr.on("data", (data) => {
    output += data;
  });
  return {
    exit: new Promise((resolve) => child.once("exit", (code) => resolve({ code, output }))),
    kill: () => child.exitCode === null && child.kill("SIGKILL"),
  };
}

test("independent client pairs through the web PIN on the private fake host", async ({ page }) => {
  const missing = fakeHostEnv.filter((name) => !process.env[name]);
  if (missing.length && process.env.CI)
    throw new Error(`CI must provide ${missing.join(", ")} for the fake-host pairing journey`);
  test.skip(
    missing.length > 0,
    `Set ${missing.join(", ")}: binds an RFC1918 address, so it does not run under a loopback-only policy`,
  );
  test.setTimeout(90000);
  const directory = await mkdtemp(path.join(scratch, "pairing-fake-host-"));
  const config = await mkdtemp(path.join(scratch, "pairing-client-"));
  // The host's durable storage only opens an existing owner-only (0700) directory; it is
  // reused below so the restarted host proves persisted pairing.
  await mkdir(path.join(directory, "state"), { mode: 0o700 });
  // Pairing never streams: placeholder media bytes only satisfy argument checks.
  await writeFile(path.join(directory, "video.h264"), Buffer.from([0, 0, 0, 1, 9, 0xf0]));
  await writeFile(path.join(directory, "audio.opus"), Buffer.alloc(16));
  let host = await fakeHost(directory);
  const pin = randomPin();
  let pairing;
  try {
    pairing = client(["--config-dir", config, "pair", "--host", host.bind, "--pin", pin]);
    await host.event("pair_pending", 20000);
    await page.goto(host.url);
    await expect(page.locator("#pin-form")).toBeVisible();
    await page.getByLabel("Four-digit PIN").click();
    await page.getByLabel("Four-digit PIN").fill(pin);
    await page.getByLabel("Device name (optional)").fill("Den phone");
    const submitted = Date.now();
    await page.getByRole("button", { name: "Submit PIN" }).click();
    await expect(page.locator("#pairing-state")).toContainText("Paired Den phone", {
      timeout: 3000,
    });
    expect(Date.now() - submitted).toBeLessThanOrEqual(3000);
    const paired = await pairing.exit;
    expect(paired.code, paired.output.replaceAll(pin, "[redacted]")).toBe(0);
    host.command("status");
    expect((await host.event("status")).paired).toBe(1);
    expect(await overflow(page)).toBeLessThanOrEqual(0);
    await host.close();
    host = await fakeHost(directory);
    await page.goto(host.url);
    await page.getByRole("button", { name: "Paired devices" }).click();
    await expect(page.locator("#devices article")).toHaveCount(1);
    await expect(page.locator("#devices")).toContainText("Den phone");
    const apps = await client(["--config-dir", config, "apps", "--host", host.bind]).exit;
    expect(apps.code, apps.output).toBe(0);
  } finally {
    pairing?.kill();
    await host.close();
    await rm(directory, { recursive: true, force: true });
    await rm(config, { recursive: true, force: true });
  }
});
