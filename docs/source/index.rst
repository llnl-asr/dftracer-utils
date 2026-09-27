:layout: landing
:description: A C++20 engine (plus C ABI and Python bindings) for reading, indexing, and analyzing DFTracer traces at scale.

.. raw:: html

   <div class="dft-hero">
     <img class="dft-hero-banner dft-banner-light" src="_static/logo-banner-light.svg" alt="DFTracer Utils" />
     <img class="dft-hero-banner dft-banner-dark" src="_static/logo-banner-dark.svg" alt="DFTracer Utils" />
     <h1>Analyze DFTracer traces at scale</h1>
     <p class="dft-tagline">A coroutine-based C++20 engine with a stable C ABI and
     Python bindings: read and index <code>.pfw.gz</code> traces,
     run a SIMD columnar DataFrame, compose async pipelines, and author plugins -
     the same capabilities from C, C++, and Python.</p>
   </div>

.. grid:: 1 2 3 5
   :gutter: 3
   :class-container: sd-mb-4

   .. grid-item-card:: :octicon:`rocket` Get started
      :link: getting-started/index
      :link-type: doc

      Install it and run your first query, pipeline, and plugin. Start here if
      you are new.

   .. grid-item-card:: :octicon:`mortar-board` Tutorials
      :link: tutorials/index
      :link-type: doc

      Guided, end-to-end lessons: your first analysis, C++, serving, scaling
      out, and writing a plugin.

   .. grid-item-card:: :octicon:`book` Guides
      :link: guides/index
      :link-type: doc

      Task-oriented how-tos: columnar ops, duql filters, fan-out/fan-in,
      channels, plugin authoring.

   .. grid-item-card:: :octicon:`code` Reference
      :link: reference/index
      :link-type: doc

      The complete API: Python, C++, and the C ABI, generated from the source.

   .. grid-item-card:: :octicon:`light-bulb` Concepts
      :link: concepts/index
      :link-type: doc

      How it works: the fold-fusion engine, the coroutine runtime, and the
      plugin and compose models.

What it does
------------

.. grid:: 1 1 3 3
   :gutter: 2

   .. grid-item-card:: Columnar, in three languages

      A SIMD ``DataFrame``/``Series`` engine (Highway kernels) with arithmetic,
      reducers, strings, joins, and reshaping - reachable from C, C++, and
      Python, zero-copy across Arrow and NumPy.

   .. grid-item-card:: Async by construction

      A C++20 coroutine runtime (io_uring on Linux, kqueue on macOS) with
      pipelines, task graphs, channels, and structured concurrency.

   .. grid-item-card:: Extensible

      Author plugins against a stable C ABI, a C++ SDK, or a Python JIT DSL, and
      build queries and compose ops that stay in the engine.

Use with an AI assistant
------------------------

This site publishes ``llms.txt`` (a map of the docs) and ``llms-full.txt``
(every page inlined), following the
`llms.txt convention <https://llmstxt.org/>`_. Paste this into your AI assistant:

.. raw:: html

   <div class="llm-assist">
     <pre id="llm-prompt" class="llm-prompt">Loading...</pre>
     <button type="button" id="llm-copy" class="llm-copy">Copy</button>
     <p class="llm-links">
       Open directly: <a id="llm-link" href="llms.txt">llms.txt</a> |
       <a id="llm-link-full" href="llms-full.txt">llms-full.txt</a>
     </p>
   </div>
   <script>
   (function () {
     var base = window.location.href.replace(/[^/]*$/, "");
     var url = new URL("llms.txt", base).href;
     var fullUrl = new URL("llms-full.txt", base).href;
     var prompt =
       "Read " + url + " to learn dftracer-utils and its documentation, then " +
       "help me install it and work through my task step by step. For the full " +
       "docs in one file, use " + fullUrl + ".";
     var pre = document.getElementById("llm-prompt");
     if (pre) { pre.textContent = prompt; }
     var link = document.getElementById("llm-link");
     if (link) { link.href = url; }
     var linkFull = document.getElementById("llm-link-full");
     if (linkFull) { linkFull.href = fullUrl; }
     var btn = document.getElementById("llm-copy");
     if (btn && navigator.clipboard) {
       btn.addEventListener("click", function () {
         navigator.clipboard.writeText(prompt).then(function () {
           btn.textContent = "Copied";
           setTimeout(function () { btn.textContent = "Copy"; }, 1500);
         });
       });
     } else if (btn) {
       btn.style.display = "none";
     }
   })();
   </script>

Citation
--------

If you use dftracer-utils in your research, please cite the HORATIO paper:

   Ray A. O. Sinurat, William Nixon, Haryadi S. Gunawi, Nikoli Dryden, and
   Hariharan Devarajan. 2026. HORATIO: Bridging Management and Analysis of Traces
   at Scale. In The International Conference on Scalable Scientific Data
   Management 2026 (SSDBM 2026). ACM.
   doi:\ `10.1145/3828820.3828825 <https://doi.org/10.1145/3828820.3828825>`_

See :doc:`citation` for the BibTeX entry.

.. admonition:: Where the name comes from
   :class: tip

   HORATIO is named after Horatius Cocles, who held the Tiber bridge to defend
   Rome. The name echoes the HORATIO's role to bridge trace management and
   analysis while keeping the raw traces intact.

.. toctree::
   :hidden:
   :caption: Get started

   getting-started/index

.. toctree::
   :hidden:
   :caption: Tutorials

   tutorials/index

.. toctree::
   :hidden:
   :caption: Guides

   guides/index

.. toctree::
   :hidden:
   :caption: Reference

   reference/index

.. toctree::
   :hidden:
   :caption: Concepts

   concepts/index

.. toctree::
   :hidden:
   :caption: More

   cli
   environment
   developers
   changelog
   citation
   open-source
