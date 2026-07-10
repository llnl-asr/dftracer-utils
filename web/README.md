# DFTracer trace viewer (web UI)

A self-contained Perfetto-style timeline for `dftracer_server`. SolidJS + Canvas,
bundled by Vite into a single `dist/index.html` (all JS/CSS inlined) that the C++
build embeds into the `dftracer_server` binary.

## Develop

```bash
npm install
npm run dev        # dev server on :5173, proxies /api to a server on :8080
```

Run a backend in another terminal: `dftracer_server -p 8080 <trace-dir>`.

## Build the embedded bundle

```bash
npm run build      # writes dist/index.html (single file)
```

`dist/index.html` is committed so the C++/cluster build never needs Node. After
changing the UI, rebuild it and commit the result. From CMake you can instead run
the `web-ui` target, or configure with `-DDFTRACER_UTILS_BUILD_WEB_UI=ON` to
rebuild it automatically during the server build (requires npm).

## Backend endpoints used

- `GET /api/v1/info` - global time bounds, file count.
- `GET /api/v1/viz/events?begin&end&summary&query&limit` - windowed events
  (Chrome Trace Event Format) with a raw DSL `query` filter.

## Security

The server is a read-only viewer with no built-in TLS. A trace can expose
hostnames, full file paths, and command lines, so treat it as sensitive.

- **Bind address.** Defaults to `127.0.0.1` (loopback only). Access a remote
  trace over an SSH tunnel (`ssh -L 8080:localhost:8080 host`). Only pass
  `-b 0.0.0.0` when you deliberately want it reachable on the network.
- **Access token.** `--token <secret>` requires every request to carry the
  token, either as `?token=<secret>` or an `Authorization: Bearer <secret>`
  header; otherwise the request gets `401`. Optional - without it the server is
  open. Open the UI as `http://host:port/?token=<secret>`; the page forwards the
  token on its API calls. The token is compared in plaintext, so it only
  protects a trusted network or a connection already secured by TLS.
- **TLS / internet exposure.** There is no HTTPS. For anything beyond localhost
  or a private cluster network, put it behind a reverse proxy (e.g. nginx) that
  terminates TLS and can add auth and rate limiting.
- **Untrusted input.** The `query` DSL is a validated event-field filter (not
  SQL/shell); `?file=` is an exact lookup into the indexed set (no path
  traversal); request size and header count are bounded. All routes are `GET`.
