import { render } from "solid-js/web";
import { ApiExplorer } from "./api/ApiExplorer";
import "./styles.css";

// Standalone /api page: the interactive API explorer as its own document,
// embedded into dftracer_server the same way as the timeline UI.
// The viewer locks the viewport (body overflow:hidden); this standalone page
// scrolls normally instead.
document.documentElement.classList.add("api-page");

const root = document.getElementById("root");
if (root)
  render(
    () => (
      <div class="api-body api-standalone">
        <ApiExplorer />
      </div>
    ),
    root,
  );
