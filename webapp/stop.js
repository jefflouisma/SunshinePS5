export function mountStop(controls) {
  document.querySelector("#stop-button").addEventListener("click", () =>
    controls.confirm(
      "Stop SunshinePS5?",
      "End the active stream and stop the host. The foreground game stays open.",
      "Confirm stop",
      () =>
        controls.action(async () => {
          const response = await controls.api("/host/stop", "POST", { confirm: true });
          if (response.state !== "stopping")
            throw new Error("Stop acknowledgement was not received. Refresh status.");
          controls.feedback(
            "Stopping… Waiting for the host to become unavailable; final cleanup is unverified.",
          );
        }),
    ),
  );
}
