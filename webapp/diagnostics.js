import { downloadCapture } from "./metrics.js";
export async function mount(controls) {
  const root = document.querySelector("#diagnostics");
  if (!root) return;
  const heading = document.createElement("h2");
  heading.textContent = "Diagnostics";
  const description = document.createElement("p");
  description.textContent =
    "Downloads public recent events, recorded session-end reasons, versions and the current metrics capture. Keys, PINs and tokens are excluded.";
  const button = document.createElement("button");
  button.type = "button";
  button.textContent = "Download diagnostics";
  const result = document.createElement("p");
  result.setAttribute("role", "status");
  button.addEventListener("click", async () => {
    button.disabled = true;
    try {
      await downloadCapture("/diagnostics", "diagnostics.ndjson", true);
      result.textContent = "Diagnostics downloaded.";
    } catch (error) {
      result.textContent = `Download failed or incomplete: ${error.message}`;
      controls.feedback?.(result.textContent, true);
    } finally {
      button.disabled = false;
    }
  });
  root.replaceChildren(heading, description, button, result);
}
