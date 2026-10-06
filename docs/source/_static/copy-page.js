// "Copy page" / "Download page" control for dftracer-utils docs. Each page's
// Markdown is written to a sibling <page>.md (see _inject_copy_page in conf.py).
// Copy fetches it from the same origin - so it works with a private source repo
// and behind a firewall, where an external agent could not reach it. Download is
// a native <a download>, so the reader hands the .md to any agent's file upload.
// "Copy for agent" copies a short prompt with the absolute URLs of this page's
// Markdown and of llms.txt, for an agent that can fetch the web.
(function () {
  "use strict";

  function flash(root, msg) {
    var btn = root.querySelector(".dftu-copy-page-btn");
    if (!btn) return;
    var prev = btn.textContent;
    btn.textContent = msg;
    btn.disabled = true;
    setTimeout(function () {
      btn.textContent = prev;
      btn.disabled = false;
    }, 1600);
  }

  function copyPage(root) {
    var url = root.getAttribute("data-md");
    if (!url || !(navigator.clipboard && navigator.clipboard.writeText)) {
      flash(root, "Copy unavailable");
      return;
    }
    fetch(url)
      .then(function (r) {
        if (!r.ok) throw new Error(r.status);
        return r.text();
      })
      .then(function (text) {
        return navigator.clipboard.writeText(text);
      })
      .then(
        function () {
          flash(root, "Copied");
        },
        function () {
          flash(root, "Copy failed");
        }
      );
  }

  function copyForAgent(root) {
    var md = root.getAttribute("data-md");
    if (!md || !(navigator.clipboard && navigator.clipboard.writeText)) {
      flash(root, "Copy unavailable");
      return;
    }
    var base = document.documentElement.getAttribute("data-content_root") || "./";
    var page = new URL(md, window.location.href).href;
    var index = new URL("llms.txt", new URL(base, window.location.href)).href;
    var h1 = document.querySelector("h1");
    var title = h1 ? h1.textContent.replace(/[#\u00b6]/g, "").trim() : document.title;
    var text =
      'I am using dftracer-utils. Read the documentation page "' +
      title +
      '" first:\n' +
      page +
      "\n\nThe index of the whole documentation is at " +
      index +
      ".\nFetch more pages from it when you need them.\n";
    navigator.clipboard.writeText(text).then(
      function () {
        flash(root, "Copied");
      },
      function () {
        flash(root, "Copy failed");
      }
    );
  }

  function setMenu(root, open) {
    var menu = root.querySelector(".dftu-copy-page-menu");
    var toggle = root.querySelector(".dftu-copy-page-toggle");
    if (!menu || !toggle) return;
    menu.hidden = !open;
    toggle.setAttribute("aria-expanded", open ? "true" : "false");
  }

  function closeAllMenus(except) {
    document.querySelectorAll("[data-dftu-copy-page]").forEach(function (r) {
      if (r !== except) setMenu(r, false);
    });
  }

  document.addEventListener("click", function (e) {
    var root = e.target.closest("[data-dftu-copy-page]");
    if (!root) {
      closeAllMenus(null);
      return;
    }

    if (e.target.closest(".dftu-copy-page-toggle")) {
      var menu = root.querySelector(".dftu-copy-page-menu");
      var willOpen = menu ? menu.hidden : false;
      closeAllMenus(root);
      setMenu(root, willOpen);
      return;
    }

    var action = e.target.closest("[data-action]");
    if (action && action.getAttribute("data-action") === "copy") {
      setMenu(root, false);
      copyPage(root);
      return;
    }
    if (action && action.getAttribute("data-action") === "agent") {
      setMenu(root, false);
      copyForAgent(root);
      return;
    }
    // Download is a native <a download>; just let it through and close the menu.
    setMenu(root, false);
  });

  document.addEventListener("keydown", function (e) {
    if (e.key === "Escape") closeAllMenus(null);
  });
})();
