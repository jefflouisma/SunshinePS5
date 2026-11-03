const channels = [
  ["capture", "Capture"],
  ["encode", "Encode"],
  ["packetize", "Packetize"],
  ["send", "Send"],
  ["full_pipeline", "Full pipeline"],
  ["input_delivery", "Input delivery"],
];
function node(tag, text) {
  const element = document.createElement(tag);
  if (text !== undefined) element.textContent = text;
  return element;
}
function valid(q) {
  return q?.validity === "valid" && Number.isSafeInteger(q.value_us) && q.value_us >= 0;
}
function quantile(q) {
  if (valid(q)) return `${q.value_us} µs`;
  const lower = q?.lower_bound_us;
  return `Unavailable${Number.isSafeInteger(lower) ? ` (≥ ${lower} µs)` : ""}: ${q?.reason || "no observations"}`;
}
function pair(list, key, value) {
  list.append(node("dt", key), node("dd", value));
}
function interval(context) {
  const n = context?.fps_numerator;
  const d = context?.fps_denominator;
  if (!Number.isSafeInteger(n) || !Number.isSafeInteger(d) || n <= 0 || d <= 0) return null;
  return { n: BigInt(n), us: 1000000n * BigInt(d), label: `${n}/${d} fps` };
}
function render(root, response) {
  root.replaceChildren();
  const capture = response.capture;
  root.dataset.sessionId = capture?.session_id || "";
  root.append(node("h2", "Performance details"));
  if (!capture) {
    root.append(node("p", "No session capture is available. Start a session to collect metrics."));
    return;
  }
  const data = capture.data || capture;
  const context = data.context;
  const budget = interval(context);
  root.append(
    node("p", capture.synthetic ? "Synthetic fixture observations" : "Host observations"),
  );
  root.append(
    node(
      "p",
      budget
        ? `Frame interval: ${budget.us}/${budget.n} µs · ${budget.label}.`
        : "Frame interval unavailable.",
    ),
  );
  root.append(
    node(
      "p",
      `Requested target: ${context?.requested_bitrate_kbps ?? "unavailable"} kbps · chosen target: ${context?.chosen_bitrate_kbps ?? "unavailable"} kbps · ${capture.bitrate_adjustment || context?.bitrate_adjustment || "unavailable"}.`,
    ),
  );
  const coverage = capture.coverage;
  root.append(
    node(
      "p",
      `Coverage: ${coverage?.state || "unavailable"} · ${coverage?.reason || "enabled observations only"}. Gaps: ${coverage?.gaps?.length ?? "unavailable"}.`,
    ),
  );
  for (const [window, title] of [
    ["last_10_seconds", "Last ten seconds"],
    ["whole_session", "Whole session"],
  ]) {
    const section = node("section");
    section.append(node("h3", title));
    const bounds = capture.windows?.[window];
    if (bounds) section.append(node("p", `Actual window: ${bounds.start_us}–${bounds.end_us} µs.`));
    const summaries = data.summaries?.filter((item) => item.window === window) || [];
    const stages = summaries.filter((item) =>
      channels.slice(0, 4).some(([id]) => id === item.channel),
    );
    const available = stages.filter((item) => valid(item.p99));
    const maximum = available.length
      ? Math.max(...available.map((item) => item.p99.value_us))
      : null;
    if (available.length !== 4)
      section.append(
        node("p", "Stage comparison is incomplete; missing measurements are unavailable."),
      );
    for (const [id, name] of channels) {
      const summary = summaries.find((item) => item.channel === id);
      const article = node("article");
      article.dataset.channel = id;
      article.dataset.window = window;
      article.append(node("h4", name));
      const values = node("dl");
      pair(values, "Median", quantile(summary?.median));
      pair(values, "p95", quantile(summary?.p95));
      pair(values, "p99", quantile(summary?.p99));
      const population = summary?.population;
      pair(
        values,
        "Population",
        population
          ? `Seen ${population.seen}; represented ${population.represented}; missing ${population.missing}; overflow ${population.overflow}; failed delivery ${population.failed_deliveries}.`
          : "Unavailable",
      );
      article.append(values);
      if (id !== "full_pipeline" && id !== "input_delivery" && valid(summary?.p99) && budget) {
        const share = Number((BigInt(summary.p99.value_us) * budget.n * 10000n) / budget.us) / 100;
        const meter = node("meter");
        meter.min = 0;
        meter.max = 100;
        meter.value = Math.min(share, 100);
        meter.setAttribute("aria-label", `${name} p99 frame budget share`);
        article.append(meter, node("p", `p99 uses ${share}% of the frame budget.`));
        if (summary.p99.value_us === maximum) {
          article.dataset.largest = "true";
          article.append(
            node(
              "strong",
              available.length === 4
                ? "Largest stage p99 share (all ties shown)"
                : "Largest available stage p99 share",
            ),
          );
        }
      }
      if (id === "full_pipeline") {
        article.append(
          node(
            "p",
            "Measured from capture start to final network submission; stage percentiles are never added.",
          ),
        );
        if (valid(summary?.p99) && budget && BigInt(summary.p99.value_us) * budget.n > budget.us) {
          article.dataset.overBudget = "true";
          article.append(node("strong", "Full pipeline exceeds frame budget"));
        } else if (
          budget &&
          summary?.p99?.reason === "overflow" &&
          Number.isSafeInteger(summary.p99.lower_bound_us) &&
          BigInt(summary.p99.lower_bound_us) * budget.n > budget.us
        ) {
          article.dataset.overBudget = "true";
          article.append(
            node("strong", "Full pipeline lower bound exceeds frame budget; exact p99 unavailable"),
          );
        }
      }
      if (id === "input_delivery")
        article.append(
          node("p", "Input receipt to confirmed delivery; separate from frame stages."),
        );
      section.append(article);
    }
    root.append(section);
  }
  const counters = data.continuous;
  root.append(
    node(
      "p",
      `Measured encoded bitrate: ${counters?.encoded_bitrate_bps ?? "unavailable"} bps; network throughput: ${counters?.network_throughput_bps ?? "unavailable"} bps; frame rate: ${counters?.frame_rate_millihz ?? "unavailable"} mHz. Host drops: ${counters?.frame_outcomes?.[4] ?? "unavailable"}; client-reported loss: ${counters?.client_lost_packets ?? "unavailable"}; client-reported recovery requests: ${counters?.client_recovery_requests ?? "unavailable"}.`,
    ),
  );
  for (const channel of ["frames", "inputs"]) {
    const retained = data.retained?.[channel];
    root.append(
      node(
        "p",
        `Retained ${channel}: ${retained?.first_us ?? "unavailable"}–${retained?.last_us ?? "unavailable"} µs; ${retained?.records ?? "unavailable"} records. At most 600 seconds, possibly less at capacity.`,
      ),
    );
  }
}
const downloads = new Set();
function cancelDownloads() {
  for (const controller of downloads) controller.abort();
}
window.addEventListener("pagehide", cancelDownloads);
document.addEventListener("visibilitychange", () => {
  if (document.hidden) cancelDownloads();
});
export async function downloadCapture(path, filename, diagnostics = false) {
  const controller = new AbortController();
  downloads.add(controller);
  const timer = setTimeout(() => controller.abort(), 30000);
  try {
    const response = await fetch(`/api/v1${path}`, {
      cache: "no-store",
      credentials: "same-origin",
      signal: controller.signal,
    });
    if (!response.ok || !response.body)
      throw new Error("Download unavailable. Retry when the host is ready.");
    const reader = response.body.getReader();
    const chunks = [];
    const decoder = new TextDecoder("utf-8", { fatal: true });
    let total = 0;
    let pending = "";
    let header;
    let capture;
    let frames = 0;
    let inputs = 0;
    function line(text) {
      const item = JSON.parse(text);
      if (!header) {
        header = item;
        if (diagnostics && item.kind !== "diagnostics")
          throw new Error("Invalid diagnostics header.");
      }
      if (item.kind === "metrics_coverage") {
        if (capture) throw new Error("Repeated capture header.");
        capture = item;
      } else if (item.kind === "frame") frames += 1;
      else if (item.kind === "input") inputs += 1;
      else if (!(diagnostics && item.kind === "diagnostics"))
        throw new Error("Unknown download record.");
    }
    for (;;) {
      const { done, value } = await reader.read();
      if (done) break;
      total += value.byteLength;
      if (total > 32 * 1024 * 1024) {
        await reader.cancel();
        throw new Error(
          "Download exceeds this browser's 32 MiB buffer. Use a streaming download client.",
        );
      }
      chunks.push(value);
      pending += decoder.decode(value, { stream: true });
      let end = pending.indexOf("\n");
      while (end !== -1) {
        if (end > 65536) throw new Error("Download record exceeds its bound.");
        line(pending.slice(0, end));
        pending = pending.slice(end + 1);
        end = pending.indexOf("\n");
      }
      if (pending.length > 65536) throw new Error("Download record exceeds its bound.");
    }
    pending += decoder.decode();
    if (
      pending.length ||
      !header ||
      (!diagnostics && !capture) ||
      (diagnostics && header.capture_available && !capture)
    )
      throw new Error("Download is incomplete.");
    if (
      capture &&
      (frames !== capture.metrics?.retained?.frames?.records ||
        inputs !== capture.metrics?.retained?.inputs?.records)
    )
      throw new Error("Download lost retained samples.");
    const url = URL.createObjectURL(new Blob(chunks, { type: "application/x-ndjson" }));
    const link = node("a");
    link.href = url;
    link.download = filename;
    document.body.append(link);
    link.click();
    link.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  } finally {
    clearTimeout(timer);
    downloads.delete(controller);
  }
}
export async function mount(controls) {
  const root =
    document.querySelector("#metrics-view") ||
    document.querySelector("#metrics") ||
    controls.statusRoot;
  if (!root) return { dispose() {} };
  const home = document.querySelector("#metrics-control") || root;
  const heading = node("h2", "Performance metrics");
  const label = node("label", "Detailed metrics");
  const toggle = node("input");
  toggle.type = "checkbox";
  toggle.setAttribute("role", "switch");
  toggle.setAttribute("aria-label", "Detailed metrics");
  label.append(toggle);
  const state = node("p");
  state.setAttribute("role", "status");
  const button = node("button", "Download metrics capture");
  button.type = "button";
  home.replaceChildren(heading, label, state, button);
  // Keep the switch mounted and focused while snapshots replace only the details.
  const details = home === root ? node("div") : root;
  if (home === root) home.append(details);
  let timer;
  let busy = false;
  let disposed = false;
  let changing = false;
  const feedback =
    controls.feedback ||
    ((message) => {
      state.textContent = message;
    });
  async function update() {
    if (disposed || busy || document.hidden) return;
    busy = true;
    const started = performance.now();
    try {
      const value = await controls.api("/metrics");
      if (disposed || document.hidden) return;
      if (!changing) toggle.checked = value.requested;
      state.textContent = value.pending
        ? "Collection change pending (up to two seconds)."
        : value.effective
          ? "Collection enabled"
          : "Collection disabled";
      if (value.diagnostic && value.diagnostic !== "none")
        state.textContent += ` ${value.diagnostic}.`;
      render(details, value);
    } catch (error) {
      if (!disposed && !document.hidden)
        state.textContent = `Metrics unavailable: ${error.message}`;
    } finally {
      busy = false;
      if (!disposed && !document.hidden)
        timer = setTimeout(update, Math.max(0, 1000 - (performance.now() - started)));
    }
  }
  toggle.addEventListener("change", async () => {
    const enabled = toggle.checked;
    changing = true;
    toggle.disabled = true;
    try {
      await controls.api("/settings", "PATCH", { metrics_enabled: enabled });
      await controls.refresh();
    } catch (error) {
      toggle.checked = !enabled;
      feedback(error.message, true);
    } finally {
      changing = false;
      toggle.disabled = false;
      clearTimeout(timer);
      await update();
    }
  });
  button.addEventListener("click", async () => {
    button.disabled = true;
    try {
      await downloadCapture("/metrics/export", "metrics.ndjson");
      feedback("Metrics capture downloaded.");
    } catch (error) {
      feedback(`Download failed or incomplete: ${error.message}`, true);
    } finally {
      button.disabled = false;
    }
  });
  function visibility() {
    clearTimeout(timer);
    if (!document.hidden && !disposed) update();
  }
  document.addEventListener("visibilitychange", visibility);
  function dispose() {
    disposed = true;
    clearTimeout(timer);
    document.removeEventListener("visibilitychange", visibility);
  }
  window.addEventListener("pagehide", dispose, { once: true });
  await update();
  return { dispose };
}
