export function renderStatus(value) {
  const root = document.querySelector("#status");
  root.replaceChildren();
  function row(text, tag = "p", className = "") {
    const element = document.createElement(tag);
    element.textContent = text;
    if (className) element.className = className;
    root.append(element);
  }
  if (!value) {
    row("Unavailable", "h2");
    row("The host cannot be reached. Cleanup and final state are unverified.");
    return;
  }
  row(`${value.host.name} · ${value.host.state}`, "h2");
  row(`Version ${value.host.version} · Firmware ${value.host.firmware_version}`);
  row(`Address: ${value.host.address}`);
  // FR-039: unreadable saved data and unsaved revocations stay visible with a next step.
  const storage = value.storage;
  if (storage?.explanation) row(storage.explanation, "p", "warning");
  if (storage?.revocation_durability_pending)
    row(
      "A revocation is not saved; restarting would restore that device's access. Open Paired devices to retry the save.",
      "p",
      "warning",
    );
  const session = value.session;
  if (!session) {
    row("No active session");
    return;
  }
  row(`${session.device_name} · ${session.state}`, "h3");
  row(
    `${session.width} × ${session.height} · ${session.fps} fps · ${session.codec} · ${session.dynamic_range.toUpperCase()}`,
  );
  row(`Requested target: ${session.requested_bitrate_kbps} kbps`);
  row(
    `Chosen target: ${session.chosen_bitrate_kbps} kbps · ${session.bitrate_adjustment.replaceAll("_", " ")}`,
  );
  row(
    session.measured_bitrate_bps === null
      ? "Measured encoded bitrate: unavailable"
      : `Measured encoded bitrate: ${session.measured_bitrate_bps} bps (interval ${session.measured_interval_start_us}–${session.measured_interval_end_us} µs)`,
  );
  row(
    session.duration_us === null
      ? "Duration: unavailable (clock error)"
      : `Duration: ${Math.floor(session.duration_us / 1000000)} seconds`,
  );
}
