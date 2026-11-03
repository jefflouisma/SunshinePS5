export function mountSettings(controls) {
  document.querySelector("#settings-form").addEventListener("submit", (event) => {
    event.preventDefault();
    controls.action(async () => {
      const hostName = document.querySelector("#host-name").value;
      if (new TextEncoder().encode(hostName).length > 63)
        throw new Error("Name too long: use at most 63 UTF-8 bytes.");
      await controls.api("/settings", "PATCH", { host_name: hostName });
      controls.feedback("Host name saved. Refresh Moonlight to see the new name.");
    });
  });
}
