import { renderStatus } from "./status.js";
import { renderDevices } from "./devices.js";
import { mountSettings } from "./settings.js";
import { mountStop } from "./stop.js";

let tokenRequest;
let timer;
let refreshing = false;
let acting = false;
let page = "home";
let currentAbort;
let network = Promise.resolve();
let announced = null;
let formRequest = null;
const observers = [];
// Pairing requests this page showed or submitted; only their outcomes are announced.
const seenRequests = new Set();
const submittedNames = new Map();
function feedback(message, error = false, focus = error) {
  const root = document.querySelector("#feedback");
  root.textContent = message;
  root.classList.toggle("error", error);
  if (focus) root.focus();
}
function api(path, method = "GET", body, token = "") {
  const run = async () => {
    if (method === "GET" && document.hidden) throw new DOMException("Page is hidden", "AbortError");
    const controller = new AbortController();
    currentAbort = controller;
    const timeout = setTimeout(() => controller.abort(), 3000);
    try {
      const response = await fetch(`/api/v1${path}`, {
        method,
        cache: "no-store",
        credentials: "same-origin",
        signal: controller.signal,
        headers:
          method === "GET" ? {} : { "Content-Type": "application/json", "X-SP5-Token": token },
        body: body === undefined ? undefined : JSON.stringify(body),
      });
      const value = await response.json();
      if (!response.ok) {
        const error = new Error(`${value.error.message} ${value.error.recovery}`);
        error.code = value.error.code;
        throw error;
      }
      return value;
    } finally {
      clearTimeout(timeout);
      currentAbort = undefined;
    }
  };
  const result = network.then(run, run);
  network = result.catch(() => {});
  return result;
}
// One page token per page load; a failed or replaced token is fetched again only on demand.
function pageToken() {
  tokenRequest ??= api("/page").then(
    (value) => value.token,
    (error) => {
      tokenRequest = undefined;
      throw error;
    },
  );
  return tokenRequest;
}
async function request(path, method = "GET", body) {
  if (method === "GET") return api(path);
  const token = await pageToken();
  try {
    return await api(path, method, body, token);
  } catch (error) {
    if (error.code !== "stale_token") throw error;
    tokenRequest = undefined;
    await pageToken();
    throw new Error("This page was renewed and nothing was changed. Please try again.");
  }
}
function confirm(title, description, label, operation) {
  const dialog = document.querySelector("#confirmation");
  document.querySelector("#confirm-title").textContent = title;
  document.querySelector("#confirm-description").textContent = description;
  const button = document.querySelector("#confirm-action");
  button.textContent = label;
  button.onclick = () => {
    dialog.close();
    operation();
  };
  dialog.showModal();
  document.querySelector("#confirm-cancel").focus();
}
document
  .querySelector("#confirm-cancel")
  .addEventListener("click", () => document.querySelector("#confirmation").close());
function schedule() {
  clearTimeout(timer);
  if (!document.hidden) timer = setTimeout(refresh, 1000);
}
const seconds = (microseconds) => Math.ceil(microseconds / 1000000);
function outcome(last, cooldown) {
  const name = submittedNames.get(last.id) || last.client_name || "this device";
  if (last.state === "completed") return `Paired ${name}. It now appears in Paired devices.`;
  if (last.state === "expired")
    return `The pairing request from ${name} expired. Start pairing again in Moonlight.`;
  const remaining = last.attempts_before_cooldown;
  const next =
    cooldown > 0
      ? `Pairing is paused for ${seconds(cooldown)} seconds after five incorrect PINs.`
      : `Start pairing again in Moonlight. ${remaining} ${remaining === 1 ? "attempt remains" : "attempts remain"} before a 60-second pause.`;
  return `Pairing ${name} failed: the PIN did not match, or Moonlight stopped before finishing. ${next}`;
}
function renderPairing(pairing) {
  const form = document.querySelector("#pin-form");
  const state = document.querySelector("#pairing-state");
  const pending = pairing.pending;
  const cooldown = pairing.cooldown_remaining_us;
  const awaitingPin = Boolean(pending && !pending.pin_entered);
  if (pending) seenRequests.add(pending.id);
  // A PIN typed for one request is never carried over to a different request.
  if (awaitingPin && pending.id !== formRequest) {
    if (form.dataset.requestId)
      feedback(
        `A new pairing request from ${pending.client_name || "a device"} replaced the previous one. Enter the PIN Moonlight shows now.`,
        true,
      );
    document.querySelector("#pin").value = "";
    formRequest = pending.id;
  }
  form.dataset.requestId = awaitingPin ? pending.id : "";
  form.hidden = !awaitingPin;
  const last = pairing.last && seenRequests.has(pairing.last.id) ? pairing.last : null;
  if (cooldown > 0)
    state.textContent = `Pairing is paused after five incorrect PINs. Try again in ${seconds(cooldown)} seconds.`;
  else if (pending?.pin_entered)
    state.textContent = `PIN sent for ${pending.client_name || "this device"}. Finish pairing in Moonlight; this page will confirm.`;
  else if (pending)
    state.textContent = `Pair ${pending.client_name || "a device"} · expires in ${seconds(pending.expires_in_us)} seconds`;
  else if (last) state.textContent = outcome(last, cooldown);
  else state.textContent = "Start pairing in Moonlight to enter a PIN here.";
  if (last && announced !== last.id) {
    announced = last.id;
    feedback(outcome(last, cooldown), last.state !== "completed");
  }
}
async function refresh() {
  if (refreshing || acting || document.hidden) return;
  refreshing = true;
  try {
    let status;
    try {
      status = await api("/status");
    } catch (error) {
      // Only an unreachable status endpoint means the host is unavailable.
      if (!document.hidden) {
        renderStatus(null);
        document.querySelector("#status").dataset.error = error.message;
      }
      return;
    }
    if (document.hidden) return;
    renderStatus(status);
    if (document.activeElement !== document.querySelector("#host-name"))
      document.querySelector("#host-name").value = status.settings.host_name;
    for (const observer of observers) observer(status);
    if (page === "devices" && !document.hidden) renderDevices(await api("/devices"), controls);
    if (page === "home" && !document.hidden) {
      try {
        renderPairing(await api("/pairing"));
      } catch (error) {
        document.querySelector("#pairing-state").textContent = error.message;
      }
    }
  } catch (error) {
    // Background polls never move focus; the next poll retries.
    if (!document.hidden && error.name !== "AbortError") feedback(error.message, true, false);
  } finally {
    refreshing = false;
    schedule();
  }
}
async function action(operation) {
  if (acting) return;
  acting = true;
  clearTimeout(timer);
  try {
    await operation();
  } catch (error) {
    feedback(error.message, true);
  } finally {
    acting = false;
    await refresh();
  }
}
const controls = {
  api: request,
  refresh,
  action,
  feedback,
  confirm,
  statusRoot: document.querySelector("#metrics-view"),
  onStatus: (observer) => observers.push(observer),
};
mountSettings(controls);
mountStop(controls);
for (const name of ["home", "devices", "settings"])
  document.querySelector(`#${name}-button`).addEventListener("click", () => {
    page = name;
    for (const section of ["home", "devices", "settings"])
      document.querySelector(`#${section}`).hidden = section !== name;
    refresh();
  });
document.querySelector("#pin-form").addEventListener("submit", (event) => {
  event.preventDefault();
  const id = event.currentTarget.dataset.requestId;
  if (!id) return;
  action(async () => {
    const body = { request_id: id, pin: document.querySelector("#pin").value };
    const name = document.querySelector("#pair-name").value;
    if (name) body.device_name = name;
    await request("/pairing/pin", "POST", body);
    seenRequests.add(id);
    if (name) submittedNames.set(id, name);
    document.querySelector("#pin").value = "";
    feedback(
      "PIN submitted. Finish pairing in Moonlight; this page will confirm when it completes.",
    );
  });
});
document.addEventListener("visibilitychange", () => {
  clearTimeout(timer);
  if (document.hidden) currentAbort?.abort();
  else refresh();
});
window.addEventListener("pagehide", () => {
  clearTimeout(timer);
  currentAbort?.abort();
});
// Initial loads are started below; only a back/forward-cache restore refreshes here.
window.addEventListener("pageshow", (event) => {
  if (event.persisted) refresh();
});
const extensions = [
  ["#metrics-control", () => import("./metrics.js")],
  ["#diagnostics", () => import("./diagnostics.js")],
];
async function start() {
  pageToken().catch((error) => {
    if (error.name !== "AbortError")
      feedback(`Changes are unavailable until the page reloads. ${error.message}`, true);
  });
  await refresh();
  for (const [selector, load] of extensions) {
    try {
      const extension = await load();
      await extension.mount(controls);
    } catch (error) {
      document.querySelector(selector).dataset.unavailable = error.message;
    }
  }
}
start();
