let previous = "";
export function renderDevices(value, controls) {
  const root = document.querySelector("#devices");
  const fingerprint = JSON.stringify(value);
  if (previous === fingerprint) return;
  const editing = root.contains(document.activeElement) && document.activeElement.matches("input");
  const removed = [...root.querySelectorAll("article")].some(
    (article) => !value.devices.some((device) => device.id === article.dataset.deviceId),
  );
  if (editing && !removed) return;
  if (editing)
    controls.feedback("The paired-device list changed. Unsaved edits were discarded.", true);
  previous = fingerprint;
  const heading = document.createElement("h2");
  heading.textContent = "Paired devices";
  root.replaceChildren(heading);
  // A revocation whose save failed is in effect now but would be undone by a restart.
  const unsaved = value.revocation_durability_pending ? value.pending_revocations : [];
  if (value.revocation_durability_pending) {
    const warning = document.createElement("div");
    warning.className = "warning";
    warning.setAttribute("role", "alert");
    const names = unsaved.map((device) => device.name).join(", ") || "a revoked device";
    const message = document.createElement("p");
    message.textContent = `A revocation is not saved: restarting the host would restore access for ${names}. Check storage, then retry the save.`;
    warning.append(message);
    if (unsaved.length > 0) {
      const retry = document.createElement("button");
      retry.type = "button";
      retry.textContent = "Retry save";
      retry.addEventListener("click", () =>
        controls.action(async () => {
          try {
            await controls.api(`/devices/${unsaved[0].id}`, "DELETE", { confirm: true });
            controls.feedback("Revocation saved. It stays in effect after a restart.");
          } finally {
            previous = "";
          }
        }),
      );
      warning.append(retry);
    }
    root.append(warning);
  }
  for (const device of value.cleanup_pending_revocations || []) {
    const warning = document.createElement("div");
    warning.className = "warning";
    warning.setAttribute("role", "alert");
    const message = document.createElement("p");
    message.textContent = `Session cleanup is incomplete for ${device.name}. Access is blocked. Retry to finish releasing its resources.`;
    const retry = document.createElement("button");
    retry.type = "button";
    retry.textContent = "Retry cleanup";
    retry.addEventListener("click", () =>
      controls.confirm(
        "Retry revocation cleanup?",
        `Finish ending the session for ${device.name}? Its access will stay blocked.`,
        "Confirm retry",
        () =>
          controls.action(async () => {
            try {
              await controls.api(`/devices/${device.id}`, "DELETE", { confirm: true });
              controls.feedback("Session cleanup completed. Revocation is saved.");
            } finally {
              previous = "";
            }
          }),
      ),
    );
    warning.append(message, retry);
    root.append(warning);
  }
  if (value.devices.length === 0) {
    const empty = document.createElement("p");
    empty.textContent = "No paired devices.";
    root.append(empty);
  }
  for (const device of value.devices) {
    const article = document.createElement("article");
    article.dataset.deviceId = device.id;
    const title = document.createElement("h3");
    title.textContent = device.name;
    const dates = document.createElement("p");
    dates.textContent = `Paired: ${new Date(device.paired_at / 1000).toLocaleString()} · Last session: ${device.last_session_at === null ? "not recorded" : new Date(device.last_session_at / 1000).toLocaleString()}`;
    const form = document.createElement("form");
    const label = document.createElement("label");
    label.textContent = "Device name";
    const input = document.createElement("input");
    input.value = device.name;
    input.maxLength = 63;
    input.required = true;
    label.append(input);
    const save = document.createElement("button");
    save.type = "submit";
    save.textContent = "Save name";
    form.append(label, save);
    form.addEventListener("submit", (event) => {
      event.preventDefault();
      controls.action(async () => {
        await controls.api(`/devices/${device.id}`, "PATCH", { name: input.value });
        previous = "";
        input.blur();
        controls.feedback("Device name saved.");
      });
    });
    const revoke = document.createElement("button");
    revoke.type = "button";
    revoke.className = "danger";
    revoke.textContent = "Revoke";
    revoke.addEventListener("click", () =>
      controls.confirm(
        "Revoke device?",
        `Revoke ${device.name}? Its active session will end and it must pair again.`,
        "Confirm revoke",
        () =>
          controls.action(async () => {
            try {
              await controls.api(`/devices/${device.id}`, "DELETE", { confirm: true });
              controls.feedback("Device revoked.");
            } finally {
              previous = "";
            }
          }),
      ),
    );
    article.append(title, dates, form, revoke);
    root.append(article);
  }
}
