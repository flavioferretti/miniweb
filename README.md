# MiniWeb — OpenBSD System Monitoring Server

[![OpenBSD](https://img.shields.io/badge/OpenBSD-7.8-orange.svg)](https://www.openbsd.org/)
[![License](https://img.shields.io/badge/license-BSD%203--Clause-blue.svg)](LICENSE)
[![C](https://img.shields.io/badge/language-C99-brightgreen.svg)](https://en.wikipedia.org/wiki/C99)

## Name

`miniweb` — lightweight HTTP server for OpenBSD system monitoring.

## Synopsis

```sh
./build/miniweb [-h] [-v] [-b address] [-c connections] [-f file] [-p port] [-t threads]
```

## Description

MiniWeb is a lightweight HTTP server written in C99 for OpenBSD. It provides a system monitoring dashboard, a RESTful JSON API, and an integrated manual page browser.

Runtime architecture is based on a single `kqueue(2)` dispatcher thread plus worker threads. Connections are accepted, enqueued once with `EV_DISPATCH`, processed by workers, then closed. This eliminates worker races on the same fd and avoids busy-waiting.

Security hardening is enabled from startup using `pledge(2)` and `unveil(2)`. Metrics are gathered through native interfaces (`sysctl(2)`, `getloadavg(3)`, `getifaddrs(3)`, `getmntinfo(3)`) without external commands. Manual page rendering is delegated to `mandoc(1)` with a timeout.

MiniWeb does not implement TLS directly. Production deployment is intended behind `relayd(8)` for TLS termination.

At startup, MiniWeb loads configuration from file (if found), then applies CLI overrides.

![Dashboard Screenshot](docs/screenshot.png)

## Features

- Real-time metrics: CPU load, memory, swap, disk usage, network interfaces
- Process monitoring: top 10 by CPU and top 10 by memory
- Manual page browser: search via `apropos(1)` and render with `mandoc(1)`
- OpenBSD package manager UI: search packages, inspect metadata, map file paths to owning packages, list installed files
- Security hardening with `pledge(2)` + `unveil(2)`
- High-performance event loop with `kqueue(2)` + worker thread pool
- RESTful JSON API for external integrations
- No native TLS by design (run behind `relayd(8)`)

## Quick Start

### Prerequisites

- OpenBSD 7.8 or later
- Base development tools (`clang`, `make`)
- Optional benchmark tooling: `wrk`, `gnuplot`

### Build

```sh
make clean && make
```

### Build man page

```sh
make man
```

### Run unit tests

```sh
make unit-tests
```

### Run

```sh
./build/miniweb -v
```

Open <http://127.0.0.1:9001>.

## Usage

```text
./build/miniweb [options]

  -f FILE   Config file path (default: auto-detect)
  -p PORT   Port to listen on        (default: 9001)
  -b ADDR   Address to bind to       (default: 127.0.0.1)
  -t NUM    Worker thread count      (default: 4)
  -c NUM    Max concurrent conns     (default: 1280)
  -v        Verbose logging to stderr
  -h        Show this help
```

## Endpoints

Responses follow HTTP/1.1 connection semantics: keep-alive is enabled by default for
HTTP/1.1 unless the client sends `Connection: close`.
Handlers may also force close behavior, and each connection is capped at 64 served
requests before being closed.

### Route matching and response behavior (actual runtime flow)

`route_match()` applies routing in this order:

1. exact static registrations (`init_routes()` table),
2. dynamic `GET /man/...` (requires at least two slashes after `/man/`),
3. dynamic `GET /api/man...`,
4. dynamic `GET /api/packages...`,
5. dynamic `GET /static/...`.

Anything else returns a generated HTML `404 Not Found` page.

| Route kind | Match rule | Response mechanism |
|---|---|---|
| View pages (`/`, `/docs`, `/apiroot`, `/networking`, `/packages`) | Exact `GET` match in `view_routes[]` | `view_template_handler()` -> `http_render_template()` -> HTML response from template cache |
| Metrics JSON (`/api/metrics`) | Exact `GET` | `metrics_handler()` builds a fresh JSON snapshot each request and replies `application/json` (+ CORS `*`) |
| Networking JSON (`/api/networking`) | Exact `GET` | `networking_api_handler()` builds live routes/DNS/interfaces JSON and replies `application/json` |
| Package JSON (`/api/packages/*`) | Exact listed endpoints + dynamic prefix fallback | `pkg_api_handler()` dispatches to `pkg_info(1)`-backed helpers, returns `application/json`; missing query args => `400`, unknown subpath => `404` |
| Man API (`/api/man/*`) | Dynamic prefix (`/api/man`) | `man_api_handler()` returns JSON for sections/pages/metadata endpoints and `text/plain` for search output |
| Man render (`/man/{area}/{section}/{page}[.fmt]`) | Dynamic prefix + slash-count guard | `man_render_handler()` validates tokens, uses disk cache when available, otherwise renders via `mandoc(1)` and replies by MIME (`html/pdf/ps/md/txt`) |
| Static assets (`/static/*`) | Dynamic prefix (`/static/`) | `static_handler()` blocks traversal (`..`, `//`), resolves MIME by extension, then streams through `http_send_file()` |
| Favicon (`/favicon.ico`) | Exact `GET` | `favicon_handler()` serves `{static_dir}/assets/favicon.svg` as `image/svg+xml` |

> Note: only `GET` routes are registered. Unsupported methods typically fall through to `404` (not `405`) because routing is method+path exact match.

### Web Interface

| Endpoint | Description |
|---|---|
| `GET /` | Dashboard with system overview |
| `GET /docs` | Manual page browser with `apropos(1)` search |
| `GET /networking` | Interfaces, routes, DNS configuration |
| `GET /packages` | Package manager UI (search/info/which-owner) |
| `GET /apiroot` | API index page |

### Add or Remove Regular Pages (`/`, `/docs`, ...)

Template-backed pages are declared in `view_routes[]` in `src/urls.c`.

To add a page (example: `/about`):

1. Create templates in `templates/`:
   - `about.html`
   - `about_extra_head.html` (optional)
   - `about_extra_js.html` (optional)
2. Add route entry in `src/urls.c`:

```c
{"GET", "/about", "MiniWeb - About", "about.html",
 "about_extra_head.html", "about_extra_js.html"},
```

3. Rebuild:

```sh
make clean && make
./build/miniweb -v
```

To remove a page, remove the corresponding `view_routes[]` entry and rebuild.

### Metrics API

| Endpoint | Response |
|---|---|
| `GET /api/metrics` | Full JSON system snapshot |
| `GET /api/networking` | Interfaces, routes, resolver JSON |

`/api/metrics` includes hostname, OS version, uptime, load averages, memory, swap, per-filesystem disk usage, network status, top process lists (CPU/memory), and process counters.

### Packages API

| Endpoint | Description |
|---|---|
| `GET /api/packages/search?q={query}` | Search installed packages by name pattern (`pkg_info -Q`). |
| `GET /api/packages/info?name={package}` | Show detailed package metadata for one package (`pkg_info`). |
| `GET /api/packages/which?path={absolute_file_path}` | Find package owning an absolute path (`pkg_info -E`). |
| `GET /api/packages/files?name={package}` | List files installed by a package (`pkg_info -L`). |
| `GET /api/packages/list` | List all installed packages (`pkg_info -q`). |

### Manual Pages API

| Endpoint | Description |
|---|---|
| `GET /api/man/search?q=query` | Raw `apropos(1)` output (`text/plain`) |
| `GET /man/{area}/{section}/{page}` | HTML (default) |
| `GET /man/{area}/{section}/{page}.html` | HTML |
| `GET /man/{area}/{section}/{page}.md` | Markdown |
| `GET /man/{area}/{section}/{page}.pdf` | PDF |
| `GET /man/{area}/{section}/{page}.ps` | PostScript |
| `GET /man/{area}/{section}/{page}.txt` | ASCII text |

Rendered manual pages are cached for reuse under `static/man/{area}/{section}/{page}.{format}` (absolute path: `{static_dir}/man/{area}/{section}/{page}.{format}` where `static_dir` comes from config).

## Caching facilities (implemented)

MiniWeb currently uses three distinct caches:

1. **Template cache (`template_engine.c`)**
   - Preloaded once at startup by `template_cache_init()` by reading every regular file under `templates_dir`.
   - Rendering looks up template fragments in memory and duplicates strings per request.
   - No TTL/invalidation; refresh requires process restart.

2. **In-memory static file cache (`http_handler.c`)**
   - Fixed-size cache with `FILE_CACHE_SLOTS=32` and per-object max payload `FILE_CACHE_MAX_BYTES=256 KiB`.
   - Admission is **two-hit**: first hit enters candidate table, second hit allows insertion.
   - Insertion is **rate-limited** by token budget (`FILE_CACHE_INSERTS_PER_SEC=8`).
   - Both cache entries and candidates are evicted by age (`FILE_CACHE_MAX_AGE_SEC=120`).
   - Validation key is `(path, mtime)`; stale-on-disk changes invalidate hits automatically when mtime differs.

3. **On-disk man render cache (`man.c`)**
   - Cache path: `{static_dir}/man/{area}/{section}/{page}.{format}`.
   - Supported cached formats: `html`, `txt`, `md`, `ps`, `pdf`.
   - Request flow checks the cache first; on miss it renders via `mandoc`, writes cache file, then prefers serving back through static-file path.
   - Cached man files can also benefit from the in-memory static cache after repeated access.

### Static Files

| Endpoint | Description |
|---|---|
| `GET /static/*` | CSS, JS, images (rejects `..` and `//`) |
| `GET /favicon.ico` | `static/assets/favicon.svg` as `image/svg+xml` |

## Architecture

### Project Structure

```text
.
├-- Makefile
├-- README.md
├-- benchmark.sh
├-- build/
├-- docs/
├-- include/
├-- src/
├-- static/
├-- templates/
└-- tests/
```

### Codebase diagram

![Diagram](docs/miniweb_diagram.svg)

### Request Lifecycle

```text
accept()
   │
   ▼
kqueue dispatcher (main thread)
   │  EV_DISPATCH → event auto-disabled after delivery
   ▼
work queue (pthread_cond_wait, zero busy-waiting)
   │
   ▼
worker thread (1 of N)
   │  recv() → parse → route_match() → handler()
   ▼
close(fd) + free_connection()
```

Main thread handles accept + queueing only; workers never call `kevent()`. `EV_DISPATCH` ensures one worker handles one readiness event.

Internal refactoring keeps connection teardown logic in one path (`free_connection()`), and view rendering in one path (`http_render_template()`), reducing duplicated code across handlers and timeout/shutdown flows.

### Key Components

- `src/main.c`: dispatcher + pool, fd-indexed connection table, generation checks, idle timeout sweeps
- `src/urls.c`: declarative route tables (`view_routes[]`) plus grouped registration helpers for view and package API endpoints
- `src/http_handler.c`: shared response helpers, including centralized template rendering (`http_render_template`)
- `src/metrics.c`: metrics via `sysctl(2)` and other native interfaces with retry loop for `KERN_PROC_ALL`
- `src/template_engine.c`: simple `{{TOKEN}}` substitution from `templates/`

## Performance

Measured on OpenBSD 7.8, amd64, 4-core CPU with `wrk` (4 threads, 20 s per run).
Connection counts in parentheses indicate the concurrency level at which each figure was recorded.

### Static file serving

| Endpoint | Conns | Req/s | Avg latency |
|---|---:|---:|---:|
| `/static/test.html` | 32 | **23,201** | 1.47 ms |
| `/static/css/custom.css` | 32 | **18,335** | 1.62 ms |
| `/static/js/theme_toggler.js` | 32 | **17,965** | 1.68 ms |
| `/static/assets/favicon.svg` | 32 | **17,557** | 1.72 ms |

Static file throughput peaks above **23,000 req/s** at moderate concurrency. All four endpoints sustain over **17,000 req/s** at 32 concurrent connections with sub-2 ms average latency.

### Dynamic page rendering

| Endpoint | Conns | Req/s | Avg latency |
|---|---:|---:|---:|
| `/docs` | 32 | **9,615** | 3.46 ms |
| `/` | 64 | **7,851** | 8.89 ms |
| `/packages` | 32 | **6,109** | 5.36 ms |
| `/apiroot` | 32 | **5,324** | 6.03 ms |
| `/networking` | 32 | **2,030** | 19.34 ms |

Template-backed pages sustain **5,000–9,600 req/s**. `/networking` is slower because it collects live interface, routing, and DNS data on every request.

### JSON API endpoints

| Endpoint | Conns | Req/s | Avg latency |
|---|---:|---:|---:|
| `/api/networking` | 64 | **1,790** | 35.53 ms |
| `/api/metrics` | 64 | **101** | 604.94 ms |

`/api/metrics` throughput is bounded by the cost of a full system snapshot (sysctl, process table, filesystem and network enumeration) collected on every request, not by HTTP overhead.

### Package API endpoints

| Endpoint | Conns | Req/s | Avg latency |
|---|---:|---:|---:|
| `/api/packages/info` | 8 | **14** | 556 ms |
| `/api/packages/search` | 8 | **11** | 686 ms |
| `/api/packages/files` | 8 | **9** | 823 ms |

Package API throughput is bounded entirely by `pkg_info(1)` subprocess execution time. These endpoints spawn an external process per request and are not intended for high-frequency automated polling.

### Manual page endpoints

| Endpoint | Conns | Req/s (uncached) | Avg latency (uncached) |
|---|---:|---:|---:|
| `/api/man/search` | 32 | **22** | 1,370 ms |
| `/man/1/1/ls` | 32 | **4** | — |

First-render latency is dominated by `mandoc(1)` subprocess time. After the first render, pages are cached under `static/man/` and served at static-file speeds.

### History note

The rewrite from `libmicrohttpd` to native `kqueue(2)` + `EV_DISPATCH` workers improved measured static throughput from ~7,000 req/s to over **23,000 req/s** (~+228%, measured with `wrk` at 32 concurrent connections on a four-core system).

## Security

### Sandboxing

`unveil(2)` restricts to:

```text
templates/          r
static/             rwc
/usr/share/man      r
/usr/local/man      r
/usr/X11R6/man      r
/usr/bin/mandoc     x
/usr/bin/man        x
/usr/bin/apropos    x
/bin/ps             x
/usr/bin/netstat    x
/bin/sh             x
/etc/man.conf       r
/dev/null           rw
/usr/sbin/pkg_info  x     — packages API
/var/db/pkg         r     — installed package database
/usr/local          r     — pkg_info -E path resolution
/usr/bin            r
/usr/sbin           r
/bin                r
/sbin               r
/usr/local/bin      r
/etc/passwd         r
/etc/group          r
/etc/resolv.conf    r
```

`pledge(2)` promises:

```text
stdio rpath wpath cpath inet route proc exec vminfo ps getpw
```

### Additional mitigations

- Static traversal protection (`..`, `//` rejected)
- Connection cap with immediate 503 for excess clients
- Sanitization of manual page path components before `mandoc(1)` execution
- Forwarded headers trusted only from `trusted_proxy`

### Known limitations

- No built-in TLS
- No authentication layer
- Some JSON fields can be truncated by fixed-size buffers
- Timeout logic cannot preempt kernel-level stalls inside `mandoc(1)`

## Reverse Proxy

MiniWeb is designed to run behind `relayd(8)`.

When proxied, client identity/protocol are read in this order:

1. `X-Real-IP`
2. `X-Forwarded-For` (first value)
3. Socket peer address

`X-Forwarded-Proto` should be set to `https` by the proxy for original TLS connections.

### relayd example

```conf
table <miniweb> { 127.0.0.1 }

http protocol "https_proxy" {
    tls keypair "server"

    match request header set "X-Forwarded-For"   value "$REMOTE_ADDR"
    match request header set "X-Forwarded-Proto" value "https"
    match request header set "X-Real-IP"         value "$REMOTE_ADDR"
    match request header set "Host"              value "$HOST"

    tcp { nodelay }
}

relay "https_frontend" {
    listen on egress port 443 tls
    protocol "https_proxy"
    forward to <miniweb> port 9001
}

relay "http_redirect" {
    listen on egress port 80
    forward to <miniweb> port 9001
}
```

## Configuration File

Search order (first match wins):

1. `-f /path/to/file` (explicit, failure is fatal)
2. `./miniweb.conf`
3. `$HOME/.miniweb.conf`
4. `/etc/miniweb.conf`

CLI flags override file values. Unknown directives produce warnings without aborting startup.

### Format

```text
# comment
key    value
```

### Directives

| Key | Default | Description |
|---|---|---|
| `port` | `9001` | TCP port |
| `bind` | `127.0.0.1` | IPv4 bind address |
| `threads` | `4` | Worker thread count |
| `max_conns` | `1280` | Max concurrent connections |
| `conn_timeout` | `30` | Idle timeout (seconds) |
| `max_req_size` | `16384` | Max request size (bytes) |
| `mandoc_timeout` | `10` | `mandoc(1)` timeout (seconds) |
| `static_dir` | `static` | Static assets directory |
| `templates_dir` | `templates` | Templates directory |
| `mandoc_path` | `/usr/bin/mandoc` | `mandoc(1)` binary path |
| `trusted_proxy` | `127.0.0.1` | Trusted proxy IP for forwarded headers |
| `verbose` | `no` | `yes/no/true/false/1/0` |


## kqueue Concurrency Analysis (main + http_handler)

### Current strategy strengths for simultaneous requests

- `EV_DISPATCH` on client sockets prevents duplicate worker handling of the same fd after readiness delivery, reducing race and lock contention under bursty parallel traffic.
- `handle_accept()` drains the entire accept backlog in one pass, which is a good strategy for high connection fan-in.
- Workers block on `pthread_cond_wait` (no spin), and the dispatcher is the single owner of `kevent()`, which keeps scheduling predictable.
- Keep-alive rearm (`EV_ENABLE`) avoids full reconnect overhead for sequential requests.

### Latest merged PR summary (PR-41)

- Replaced per-accept `calloc/free` connection lifecycle with a fixed-size connection slab + free-stack in `main.c`.
- Added a pooled `http_response_t` allocator in `http_handler.c` (256 reusable response objects).
- Hardened static file cache behavior with:
  - insertion token budget (`FILE_CACHE_INSERTS_PER_SEC=8`),
  - two-hit admission before entering the main cache,
  - age-based eviction (`FILE_CACHE_MAX_AGE_SEC=120`) for both cache entries and admission candidates.

Why this improves efficiency even if req/s is similar:

- **Less allocator churn**: under concurrent keep-alive traffic, reusing connection/response objects cuts repeated malloc/free and reduces allocator lock contention.
- **Better CPU cache locality**: slab/pool objects are reused in-place, so hot metadata stays warmer in L1/L2.
- **Less memory pollution**: one-shot static files are less likely to evict useful cache entries thanks to admission + rate limiting.
- **More predictable tail behavior**: stale-file eviction and bounded insert rate avoid bursty memory growth and allocator spikes.

### Strategic improvements for spawn/parallel response throughput

- **Batch kevent changes**: where possible, accumulate multiple `EV_ENABLE`/`EV_ADD` operations and flush with one `kevent()` call to reduce syscall pressure.
- **Accept/load-shedding tuning**: expose and tune `LISTEN_BACKLOG`, `MAX_EVENTS`, and `QUEUE_CAPACITY` together for workload shape (short/static vs long/dynamic).
- **Backpressure visibility**: export counters for queue-full drops and keep-alive rearm failures to drive tuning from real measurements.
- **Hot response path**: for static files larger than cache threshold, consider `sendfile(2)`-style zero-copy path (platform permitting) to reduce userspace copy costs.

### Ring buffer opportunities

- **Already present**: the work queue in `main.c` is already a fixed-size circular buffer (`head/tail/count`, modulo arithmetic).
- **Good candidate**: request receive path currently resets and re-parses a linear per-connection buffer. A per-connection circular RX buffer can reduce memmove/fragmentation pressure when headers arrive in many small chunks.
- **Not a strong candidate**: `http_handler` response write path (`writev_all`) already streams with iovecs; a second ring buffer for TX would often add complexity without clear wins unless moving to async multi-response pipelining.

### Memory allocation/deallocation analysis

- **Connection lifecycle (`main.c`)**: now uses a preallocated fixed-size slab + free-stack; no per-accept heap allocation in the hot path.
- **Static file cache (`http_handler.c`)**: still allocates payload copies, but insertion is token-limited, gated by two-hit admission, and stale entries are evicted by age.
- **File-send path**: uncached file responses may still allocate a temporary full-file copy (`cache_copy`) up to cache limit for insertion.
- **Response objects**: now served from a mutex-protected pool (`256` slots), avoiding frequent heap allocation/free for `http_response_t`.

Potential follow-ups:

- evaluate lock-sharding or per-thread response pools to reduce mutex contention at very high thread counts;
- measure token budget (`FILE_CACHE_INSERTS_PER_SEC`) per workload class (static-heavy vs API-heavy) and tune accordingly;
- consider a zero-copy `sendfile(2)` path for larger static files where platform/runtime behavior is favorable.

## Hard-coded caps and effective ranges

Some limits are compile-time constants (hard caps), while others are configurable but clamped by parser/runtime rules.

### Compile-time hard caps (wired in code)

| Cap | Value | Effective range | Notes |
|---|---:|---|---|
| `MAX_EVENTS` | 64 | `1..64` per `kevent` wait | Upper bound of events processed per loop iteration. |
| `MAX_CONNECTIONS` | 1280 | `1..1280` runtime active conns | Absolute upper bound; `-c`/`max_conns` cannot exceed this. |
| `THREAD_POOL_SIZE` | 4 | `1..4` workers | Runtime `threads` is clamped to this value. |
| `REQUEST_BUFFER_SIZE` | 16384 | `1024..16384` bytes effective request size | Runtime `max_req_size` parsed up to 1 MiB but now clamped to buffer size. |
| `LISTEN_BACKLOG` | 128 | fixed (`listen(2)` backlog hint) | Pending accept queue hint; tune with kernel limits in mind. |
| `QUEUE_CAPACITY` | 512 | `1..512` queued dispatched conns | Circular queue depth before overload 503/drop path. |
| `MAX_KEEPALIVE_REQUESTS` | 64 | fixed per connection | Forces close after 64 served requests on one keep-alive connection. |
| `WRITE_RETRY_LIMIT` | 256 | fixed write retries | Max EAGAIN retry cycles before write failure. |
| `WRITE_WAIT_MS` | 100 | fixed poll wait ms | Poll wait step used by write retry loop. |
| `FILE_CACHE_SLOTS` | 32 | fixed entries | Number of in-memory static file cache slots. |
| `FILE_CACHE_MAX_BYTES` | 262144 | `1..262144` bytes per cached file | Files above threshold bypass memory cache. |
| `FILE_CACHE_INSERTS_PER_SEC` | 8 | fixed inserts/second | Token budget that rate-limits cache insertions. |
| `FILE_CACHE_MAX_AGE_SEC` | 120 | fixed seconds | Age-based eviction threshold for cache entries and admission candidates. |
| Response pool size | 256 | fixed objects | Upper bound of reusable `http_response_t` objects before allocation fails. |

### Configurable caps and parser ranges

| Key | Parser range | Runtime effective range | Reason |
|---|---|---|---|
| `threads` | `1..64` | `1..4` | hard-clamped by `THREAD_POOL_SIZE`. |
| `max_conns` | `1..65535` | `1..1280` | hard-clamped by `MAX_CONNECTIONS`. |
| `max_req_size` | `1024..1048576` | `1024..16384` | hard-clamped by `REQUEST_BUFFER_SIZE`. |
| `conn_timeout` | `1..3600` | `1..3600` | used by idle sweep logic. |
| `mandoc_timeout` | `1..120` | `1..120` | subprocess timeout bound. |
| `port` | `1..65535` | `1..65535` | standard TCP port range. |

Recommended tuning profile for high parallel request loads:

- keep `threads` near CPU core count up to hard cap;
- increase `max_conns` only with enough RAM and worker capacity;
- keep `max_req_size` as low as practical for target APIs;
- monitor 503/drop behavior to decide whether queue/backlog caps need recompilation.

## Files

- `./miniweb.conf`, `$HOME/.miniweb.conf`, `/etc/miniweb.conf` — startup config search list
- `templates/` — HTML templates
- `static/` — assets served under `/static/`
- `/etc/passwd`, `/etc/group`, `/etc/resolv.conf` — lookups/data for runtime views
- `/etc/man.conf`, `/usr/share/man`, `/usr/local/man`, `/usr/X11R6/man` — manpage data
- `/usr/bin/mandoc`, `/usr/bin/apropos`, `/bin/ps`, `/usr/bin/netstat`, `/bin/sh` — executed helpers
- `/usr/sbin/pkg_info` — package information tool, executed by packages API
- `/var/db/pkg` — installed package database, read by `pkg_info(1)`
- `/usr/local`, `/usr/bin`, `/usr/sbin`, `/bin`, `/sbin` — unveiled read-only so `pkg_info -E` can stat arbitrary file paths

## Examples

```sh
miniweb -v
miniweb -f /etc/miniweb.conf
miniweb -f /etc/miniweb.conf -p 8080
miniweb -b 0.0.0.0 -p 8080 -t 8
miniweb -b 127.0.0.1 -p 9001 -c 1280 -t 4
curl -s http://127.0.0.1:9001/api/metrics | jq .
```

Proxy smoke-check:

```sh
for p in / /docs /apiroot /networking /packages \
      /api/metrics /api/networking \
      '/api/packages/search?q=curl' \
      '/api/man/search?q=pledge' \
      /static/css/custom.css /favicon.ico; do
  printf '%-44s %s\n' "$p" \
    "$(curl -sk -o /dev/null -w '%{http_code}' "https://example.com$p")"
done
```

### OpenBSD rc.d service

Esempio di script `/etc/rc.d/miniweb`:

```sh
#!/bin/ksh

daemon="/home/flavio/DEV/miniweb/build/miniweb"
daemon_flags="-f /etc/miniweb.conf"
daemon_user="flavio"

. /etc/rc.d/rc.subr

rc_bg=YES
rc_cmd $1
```

## Development

### Testing

```sh
make unit-tests

for p in / /docs /apiroot /networking /packages \
          /api/metrics /api/networking \
          '/api/packages/search?q=curl' \
          '/api/packages/info?name=curl-8.16.0' \
          '/api/packages/list' \
          '/api/man/search?q=pledge' \
          /static/css/custom.css /favicon.ico; do
    printf '%-44s %s\n' "$p" \
      "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:9001$p")"
done

curl -s 'http://127.0.0.1:9001/api/man/search?q=pledge'
curl -s 'http://127.0.0.1:9001/api/packages/search?q=vim' | jq .
curl -s http://127.0.0.1:9001/api/metrics | jq .
```

### Debug build

```sh
make DEBUG=1
./build/miniweb -v
```

## Troubleshooting

- `bind: Address already in use`: another process already owns the port
- `pledge: Operation not permitted`: syscall outside current promise set
- `sysctl data query failed: Cannot allocate memory`: process table changed during collection
- `[write_all] Too many EAGAIN retries`: slow/unresponsive client, connection dropped
- Man pages not rendering: verify `mandoc` path + unveil permissions, run with `-v`

## Roadmap

- [ ] Add explicit method handling (`405 Method Not Allowed` + `Allow`) for non-GET requests on known paths.
- [ ] Add conditional HTTP caching support (`ETag`/`If-None-Match`, `Last-Modified`/`If-Modified-Since`) for static and cached man assets.
- [ ] Add cache observability counters (static cache hit/miss/admission/eviction and man cache hit/miss) under `/api/metrics`.
- [ ] Add live streaming endpoint for metrics updates (SSE or WebSocket) to reduce polling overhead.
- [ ] Add IPv6 listener and dual-stack configuration.
- [ ] Add Prometheus-compatible export endpoint.
- [ ] Evaluate portability layer for FreeBSD/NetBSD route + metrics collection.

## License

This project is licensed under the **BSD 3-Clause License** - see the [LICENSE](LICENSE) file for details.

### Third-Party Assets

This project includes the **Satoshi** font family, designed by [Indian Type Foundry](https://www.fontshare.com/fonts/satoshi).
- **Font License:** The font is included under the [Fontshare Personal & Commercial Use License](https://www.fontshare.com/terms).
- The font remains the property of its respective owners and is not covered by this project's BSD 3-Clause License.

## Acknowledgments

Built on OpenBSD interfaces and security model: `kqueue(2)`, `pledge(2)`, `unveil(2)`, `sysctl(2)`.

## Diagnostics

MiniWeb may emit the following diagnostics to stderr:

- `bind: Address already in use` — another process is already listening on the selected port
- `pledge: Operation not permitted` — a syscall fell outside the current promise set (`stdio rpath wpath cpath inet route proc exec vminfo ps getpw`)
- `sysctl data query failed: Cannot allocate memory` — process table changed during snapshot sizing; MiniWeb retries automatically
- `[write_all] Too many EAGAIN retries` — client remained back-pressured and connection was dropped
- `Connection limit reached, rejected fd=n` — active connection count reached `-c` limit

## See Also

`acme-client(1)`, `apropos(1)`, `curl(1)`, `mandoc(1)`, `kqueue(2)`, `pledge(2)`, `sysctl(2)`, `unveil(2)`, `getifaddrs(3)`, `getloadavg(3)`, `getmntinfo(3)`, `getpwuid(3)`, `relayd(8)`.

## Standards

HTTP handling targets HTTP/1.1 (RFC 7230–7235). Persistent connections
(keep-alive) are supported for sequential requests (no pipelining), with an
internal per-connection cap of 64 requests.

## History

MiniWeb originally used `libmicrohttpd`, then was rewritten in early 2026 around native `kqueue(2)` + `EV_DISPATCH` workers. This removed the external dependency and improved measured static throughput from ~7,000 req/s to over **23,000 req/s** (~+228%, measured with `wrk` at 32 concurrent connections on a four-core system).

The configuration file system (`miniweb.conf`) was added afterward to replace hardcoded runtime values.

Recent runtime-path improvements also added pooled response objects, two-hit/rate-limited/age-evicted static-file memory caching, and on-disk manpage render reuse under `static/man/...`.

## Authors

Flavio Ferretti `<flavio@flvbox.org>`.

## Caveats

- OpenBSD-specific design (depends on `kqueue(2)`, `pledge(2)`, `unveil(2)`, OpenBSD `sysctl(2)` MIBs)
- Manual rendering uses a `mandoc(1)` subprocess (sandboxed but still additional execution surface)
- JSON output uses fixed-size buffers and can truncate very long values

## Security Considerations

MiniWeb uses layered controls:

- `pledge(2)` to constrain syscalls
- `unveil(2)` to constrain filesystem paths and executable helpers
- static path traversal rejection (`..`, `//`)
- active connection cap with 503 for overflow
- trust of forwarded headers only via reverse proxy (`trusted_proxy`)

Do not expose MiniWeb directly to untrusted networks.

## Bugs

- `mandoc(1)` timeout control is wall-clock based and cannot preempt a kernel-level stall
- Template substitution currently performs raw token replacement (no HTML escaping stage)
- IPv6 is not yet implemented
