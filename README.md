# MiniWeb — OpenBSD C99 HTTP Server

[![OpenBSD](https://img.shields.io/badge/OpenBSD-7.8-orange.svg)](https://www.openbsd.org/)
[![License](https://img.shields.io/badge/license-BSD%203--Clause-blue.svg)](LICENSE)
[![C](https://img.shields.io/badge/language-C99-brightgreen.svg)](https://en.wikipedia.org/wiki/C99)

MiniWeb is a lightweight, OpenBSD-native HTTP server written in C99. It exposes a
system dashboard and JSON APIs for metrics, networking diagnostics, manual page
browsing, and package management. The runtime uses `kqueue(2)` for I/O
multiplexing and a fixed worker-thread pool for request processing.

MiniWeb is currently being re-engineered into an enterprise-grade general-purpose
web framework with clear modular boundaries, a module attach/detach API, a
centralised heartbeat scheduler, and a reusable SQLite3 storage layer. The
refactor plan lives in `docs/ENTERPRISE_REFACTOR_PLAN.md`. This document
describes the **current implemented state**.

---

## Table of Contents

1. [Prerequisites and build](#prerequisites-and-build)
2. [CLI](#cli)
3. [Configuration file](#configuration-file)
4. [Runtime architecture](#runtime-architecture)
5. [kqueue dispatcher](#kqueue-dispatcher)
6. [Worker pool and work queue](#worker-pool-and-work-queue)
7. [Connection pool](#connection-pool)
8. [HTTP layer](#http-layer)
9. [Routing](#routing)
10. [Adding and removing routes, modules, and web views](#adding-and-removing-routes-modules-and-web-views)
11. [Template engine and view cache](#template-engine-and-view-cache)
12. [Static file cache](#static-file-cache)
13. [Heartbeat scheduler](#heartbeat-scheduler)
14. [Modules](#modules)
15. [Storage layer](#storage-layer)
16. [OpenBSD security hardening](#openbsd-security-hardening)
17. [Logging](#logging)
18. [Endpoints reference](#endpoints-reference)
19. [Source layout](#source-layout)
20. [Enterprise refactor roadmap](#enterprise-refactor-roadmap)
21. [Development standards](#development-standards)

---

## Prerequisites and build

### Requirements

- OpenBSD 7.8 or later (kqueue, pledge, unveil required for full functionality)
- `clang` (or any C99-compliant compiler)
- BSD `make`
- Optional: `wrk`, `gnuplot` for benchmarking

### Build

```sh
make clean && make
```

### Run

```sh
./build/miniweb -v
```

Open: <http://127.0.0.1:9001>

### Unit tests

```sh
make unit-tests
```

### Install manual page

```sh
make man
```

---

## CLI

```
miniweb [options]

  -f FILE   Configuration file path (default: auto-detect, see below)
  -p PORT   TCP listen port (default: 9001)
  -b ADDR   Bind address (default: 127.0.0.1)
  -t NUM    Worker threads (default: 4, compile-time max: 32)
  -c NUM    Maximum concurrent connections (default: 1280)
  -l FILE   Log file path (default: stderr)
  -v        Enable verbose logging
  -h        Print usage and exit
```

CLI flags always take the highest priority and override both compiled-in
defaults and any configuration file value.

---

## Configuration file

MiniWeb reads a plain-text configuration file on startup. The lookup order
is (first file found wins):

1. Explicit path from the `-f` flag
2. `./miniweb.conf` (current working directory)
3. `$HOME/.miniweb.conf`
4. `/etc/miniweb.conf`

If no file is found, compiled-in defaults are used and startup continues
normally. A parse error in an explicitly supplied file (`-f`) is fatal.

### File format

One directive per line: `key  value`. Keys are case-insensitive. Lines
beginning with `#` and blank lines are ignored.

### Supported keys

| Key | Default | Description |
|---|---|---|
| `port` | `9001` | TCP listen port |
| `bind` / `bind_addr` | `127.0.0.1` | IPv4 bind address |
| `threads` | `4` | Worker thread count (clamped to 1–32) |
| `max_conns` | `1280` | Maximum concurrent connections |
| `conn_timeout` | `30` | Idle connection timeout in seconds |
| `max_req_size` | `16384` | Maximum HTTP request size in bytes |
| `mandoc_timeout` | `10` | Timeout for mandoc subprocess in seconds |
| `static_dir` | `static` | Path to the static assets directory |
| `templates_dir` | `templates` | Path to the HTML templates directory |
| `mandoc_path` | `/usr/bin/mandoc` | Path to the mandoc binary |
| `trusted_proxy` | `127.0.0.1` | IP whose `X-Forwarded-*` headers are trusted |
| `verbose` | `no` | Enable verbose logging (`yes`/`no`/`true`/`false`/`1`/`0`) |
| `log_file` | *(stderr)* | Log file path; empty means stderr |

### Example `/etc/miniweb.conf`

```
port          3000
bind          127.0.0.1
threads       4
max_conns     1280
conn_timeout  30
static_dir    /srv/miniweb/static
templates_dir /srv/miniweb/templates
verbose       no
log_file      /var/log/miniweb.log
```

---

## Runtime architecture

MiniWeb uses a **two-tier threading model**:

- **Dispatcher thread** (main): owns the `kqueue(2)` file descriptor, runs the
  accept loop, and enqueues ready connections into the work queue. It never
  blocks on I/O and never calls a route handler.
- **Worker pool** (N threads, configurable): each worker dequeues a connection,
  reads the full HTTP request, parses the request line and headers, dispatches
  the matching route handler, and either closes the socket or re-arms it for
  keep-alive.

The split keeps the dispatcher latency constant regardless of handler cost.

Compile-time hard limits (not overridable at runtime):

| Constant | Value | Meaning |
|---|---|---|
| `MAX_EVENTS` | 256 | `kevent()` batch size per iteration |
| `MAX_CONNECTIONS` | 4096 | Size of the connection pool array |
| `THREAD_POOL_SIZE` | 32 | Maximum worker threads |
| `REQUEST_BUFFER_SIZE` | 16384 | Per-connection receive buffer |
| `LISTEN_BACKLOG` | 1024 | `listen(2)` backlog |
| `QUEUE_CAPACITY` | 4096 | Work queue ring-buffer capacity |
| `MAX_KEEPALIVE_REQUESTS` | 64 | Requests per keep-alive connection |

---

## kqueue dispatcher

The dispatcher registers the listening socket with `EVFILT_READ | EV_CLEAR` so
that the backlog counter resets after every `kevent()` return. Client sockets
are registered with `EV_ADD | EV_DISPATCH`: `EV_DISPATCH` automatically
disables the event after delivery, eliminating the race where multiple workers
could receive the same file descriptor.

Each main-loop iteration uses a 1-second `timespec` timeout so that idle
connection sweeping runs at least once per second regardless of traffic. The
sweep iterates the connection pool, compares `last_activity` against
`conn_timeout`, and closes any expired connections from the main thread.

When the OS file descriptor table is full (`EMFILE`/`ENFILE`), the dispatcher
closes a reserved `/dev/null` spare file descriptor, accepts and immediately
drops one pending client to prevent the listen socket from spinning, then
reopens the spare. This keeps the event loop from busy-looping under fd
exhaustion.

On normal client sockets the dispatcher validates the `udata` pointer carried
in the `kevent` structure against the connection pool's generation counter
before dispatching. A stale pointer (from a recycled fd) is silently ignored.

---

## Worker pool and work queue

Workers are identical POSIX threads; no worker-specific state exists. They
share a single ring-buffer work queue (`work_queue_t`) protected by a mutex
and a condition variable:

- **Enqueue** (`queue_push`): called by the dispatcher, non-blocking. Returns
  `-1` if the queue is full; the connection is dropped with a 503 response.
- **Dequeue** (`queue_pop`): called by workers, blocks on `pthread_cond_wait`
  until a connection is available or the server is shutting down. Returns
  `NULL` on shutdown.
- **Shutdown**: `queue_broadcast_shutdown` signals all waiting workers so they
  can observe `running == 0` and exit cleanly. The main thread then joins each
  worker thread.

Within a worker, the read loop accumulates data from the non-blocking socket
until `\r\n\r\n` is found (indicating the end of HTTP headers). If `EAGAIN` is
returned before headers are complete, the kqueue event is re-enabled with
`EV_ENABLE` and the worker returns without closing the connection. This handles
clients that send headers in multiple TCP segments.

Keep-alive connections are re-armed after each request by calling
`try_rearm_keepalive`, which increments `requests_served`, resets the read
buffer, and re-registers the socket with `EV_ENABLE`. A connection is closed
after `MAX_KEEPALIVE_REQUESTS` (64) requests on the same socket.

---

## Connection pool

The connection pool is a flat array of `connection_t` structs indexed by file
descriptor (`fd`). Allocation and deallocation are O(1) via a free-stack
(LIFO). Each slot carries:

- `fd` — the client socket
- `addr` — peer socket address
- `buffer[REQUEST_BUFFER_SIZE]` — the receive buffer
- `bytes_read`, `created`, `last_activity`, `requests_served`
- `gen` — a generation counter that is incremented on free

The generation counter allows the dispatcher to detect stale `udata` pointers
in `kevent` structures: if `conn->gen != conn_gen[fd]`, the file descriptor
was recycled and the event is ignored.

All pool operations are protected by `conn_mutex`.

---

## HTTP layer

**`src/http/response.c`** implements the full request/response lifecycle:

- `http_response_create()` allocates a response object from a per-thread
  shard of a response pool (16 shards, 1024 slots each), avoiding a
  `malloc(3)` on every request under normal load. If the pool is exhausted,
  it falls back to `calloc(3)`.
- `http_response_send()` serialises the status line, standard headers
  (`Content-Type`, `Content-Length`, `Connection`, `Server`), optional custom
  headers, and the body using a single `writev(2)` call when a body is
  present, or `write(2)` for header-only responses.
- Write retries handle `EAGAIN`/`EWOULDBLOCK` on non-blocking sockets with
  `poll(2)` and a retry limit (`WRITE_RETRY_LIMIT` = 20).
- `http_response_free()` returns pool objects to their shard or calls
  `free(3)` for heap-allocated fallbacks.
- `http_request_get_header()` is thread-safe: it writes into
  `req->hdr_scratch` (per-request stack space) and never uses static storage.
- `http_request_get_client_ip()` honours `X-Real-IP` and `X-Forwarded-For`
  headers when the connection originates from `trusted_proxy`, falling back
  to the peer socket address.

**`src/http/utils.c`** provides subprocess execution:

- `safe_popen_read_argv()` forks, redirects stdout to a pipe, redirects
  stderr to `/dev/null` (preventing error messages from leaking into
  response bodies), and reads up to `max_size` bytes with a wall-clock
  timeout enforced by `poll(2)`. On timeout the child is killed with
  `SIGKILL` before `waitpid(2)`.
- `safe_popen_read()` is a convenience wrapper that passes the command to
  `/bin/sh -c`.

---

## Routing

Routing is now split into two layers: a low-level route registry (`src/router/url_registry.c`) and a lightweight module attach contract (`include/miniweb/router/module_attach.h`). `init_routes()` builds a `miniweb_module` list and lets each enabled module attach its own endpoints through `router_register()`. Dynamic endpoints (for example `/man/*` and `/static/*`) are tracked as prefix routes in the registry.

Routes are registered in a flat array of `struct route` (up to `MAX_ROUTES`
= 32 entries) in `src/router/url_registry.c`. `init_routes()` is called once
at startup from `main()`.

`route_match()` resolves a handler in two passes:

1. **Exact match**: linear scan of the route table comparing method and path
   strings.
2. **Dynamic prefix match** (GET only):
   - `/man/{area}/{section}/{page}[.fmt]` — matched when the path starts with
     `/man/` and contains at least two slashes after the prefix. Dispatches to
     `man_render_handler`.
   - `/api/man/...` — prefix match dispatches to `man_api_handler`.
   - `/api/packages/...` — prefix match dispatches to `pkg_api_handler`.
   - `/static/...` — prefix match dispatches to `static_handler`.

When `route_match` returns `NULL` but `route_path_known` returns non-zero
(the path exists for a different method), the worker sends `405 Method Not
Allowed` with an `Allow` header built by `route_allow_methods`. Otherwise a
`404 Not Found` is sent.

---

## Adding and removing routes, modules, and web views

### Adding an API endpoint

Register a new handler function in `src/router/url_registry.c` inside
`init_routes()`:

```c
register_route("GET", "/api/myfeature", my_feature_handler);
```

The handler must have the signature `int handler(http_request_t *req)` and
call one of the HTTP send helpers (`http_send_json`, `http_send_html`,
`http_send_error`, or `http_response_send`) before returning. Note that
`MAX_ROUTES` is currently 32; raise it in `include/urls.h` if needed.

### Adding a web view (template-backed page)

1. Add an entry to the `view_routes[]` table in `src/router/url_registry.c`:

```c
{"GET", "/mypage", "MiniWeb - My Page", "mypage.html",
 "mypage_extra_head.html", "mypage_extra_js.html"},
```

2. Create `templates/mypage.html` with the page body content. The base layout
   (`templates/base.html`) is injected automatically. Optional fragment files
   `mypage_extra_head.html` (for `<head>` additions) and
   `mypage_extra_js.html` (for deferred JavaScript) are silently skipped if
   absent.

3. The URL is automatically registered by `register_view_routes()` at startup.
   The rendered page is served through `view_template_handler`, which caches
   the rendered HTML in the hot-view cache for `HOT_VIEW_CACHE_TTL_SEC`
   seconds (10 s by default). Add the path to `g_hot_view_cache` in
   `src/router/route_table.c` if the route should be cached.

### Removing a route or view

Remove the `register_route()` call or delete the entry from `view_routes[]`.
The route will simply stop matching and requests to it will receive 404.

### Module attach API

The module attach API (`src/router/module_attach.c`) provides
`miniweb_module_attach_enabled()`, which iterates a `miniweb_module` array
and, for each entry with `enabled_by_default` set to non-zero, calls the
module's `init()` and `attach_routes()` callbacks. This is the foundation for
the planned plug-in architecture: a module declares its routes at attach time
rather than in `init_routes()`. The API is currently wired but modules are not
yet migrated onto it.

---

## Template engine and view cache

**Template engine** (`src/render/template_render.c`):

Templates are HTML files stored in the `templates_dir` directory (default:
`templates/`). At startup `template_cache_init()` reads every regular file in
the directory into a heap-allocated in-memory cache of `template_entry_t`
records. After the initial load, the cache is refreshed lazily every
`TEMPLATE_CACHE_TTL_SEC` seconds (60 s) by re-reading the directory. The
cache is protected by a single mutex.

`template_render_with_data()` assembles a full page by:

1. Loading `base.html` (the shell layout).
2. Loading the page-specific content file.
3. Optionally loading `extra_head_file` and `extra_js_file` fragments.
4. Calling `replace_all()` to substitute `{{title}}`, `{{page_content}}`,
   `{{extra_head}}`, and `{{extra_js}}` placeholders with the loaded
   fragments.

`template_render()` is a backward-compatible wrapper that renders a page with
the title `"MiniWeb"` and no extra fragments.

**Hot view cache** (`src/router/route_table.c`):

`view_template_handler` maintains a small hot-view cache of rendered HTML for
the five top-level pages (`/`, `/docs`, `/networking`, `/packages`,
`/apiroot`). Entries are valid for `HOT_VIEW_CACHE_TTL_SEC` seconds (10 s).
On a cache hit the rendered HTML is duplicated and returned without touching
the template engine. This eliminates repeated template assembly under load.

---

## Static file cache

**`src/http/response.c`** implements a sharded in-memory file cache for
static assets:

- 16 independent cache shards, each protecting 32 slots, keyed by file path
  using FNV-derived hashing.
- Files larger than `FILE_CACHE_MAX_BYTES` (256 KiB) are never cached; they
  are always served by streaming reads.
- A two-stage admission policy tracks candidate paths: a file must be
  requested at least twice before it is admitted to the hot cache, preventing
  one-shot requests from evicting frequently-used assets.
- Each shard enforces a rate limit of `FILE_CACHE_INSERTS_PER_SEC` (8)
  insertions per second using a token bucket. Insertions that exceed the
  budget are silently dropped.
- Entries older than `FILE_CACHE_MAX_AGE_SEC` (120 s) are evicted on the
  next access to the shard.
- Cache hits are validated against the file's `st_mtime`; a stale entry
  triggers a miss and the file is re-read from disk.
- In verbose mode, each shard logs per-second hit/miss/insert/throttle
  statistics via `log_debug`.

---

## Heartbeat scheduler

**`src/core/heartbeat.c`** provides a generic periodic task scheduler:

- Up to `HB_MAX_TASKS` (32) named tasks can be registered with
  `heartbeat_register()`. Each task specifies a callback function, an opaque
  context pointer, a period in seconds, and an initial delay.
- Duplicate registrations (by name) are silently ignored.
- `heartbeat_start()` spawns a single background thread that sleeps for 1
  second per iteration and fires all tasks whose `next_run` timestamp has
  been reached. Callbacks are invoked outside the lock.
- `heartbeat_stop()` sets a stop flag; the thread exits on the next iteration.
- The heartbeat thread is detached and does not need to be joined.

The scheduler is designed to replace per-module sampler threads (currently
used by the metrics and networking modules) with a single coordinated loop
in a future refactor phase. It is present in the codebase and ready for use
but modules have not yet been migrated onto it.

---

## Modules

All modules expose an HTTP handler function (signature
`int handler(http_request_t *req)`) registered in `init_routes()`.

### Metrics (`src/modules/metrics/`)

Provides `/api/metrics` (JSON) and the `/` dashboard. Collects via sysctl:
CPU percentages (`kern.cp_time`), memory and swap stats (`vm.uvmexp`), load
averages (`vm.loadavg`), OS info (`kern.ostype`, `kern.osrelease`,
`hw.machine`), uptime (`kern.boottime`), hostname (`kern.hostname`), disk
usage (`getmntinfo(3)`), open ports (via `netstat`), network interfaces
(`getifaddrs(3)`), and top processes by CPU (via `ps`). All JSON is
serialised with `json_escape_string()`.

### Networking (`src/modules/networking/`)

Provides `/api/networking` (JSON) and the `/networking` view. Collects
routing table entries (via `sysctl` with `NET_RT_DUMP`), DNS configuration
(`/etc/resolv.conf`), per-interface packet/byte statistics
(`SIOCGIFDATA`/`SIOCGIFFLAGS`), and active TCP/UDP connections (via
`netstat`).

### Manual pages (`src/modules/man/`)

Provides `/man/{area}/{section}/{page}[.fmt]` (rendered output) and the
`/api/man/...` JSON namespace. Renders man pages by forking `mandoc(1)` via
`safe_popen_read_argv()` with a configurable timeout (`mandoc_timeout`).
Supported output formats include `html`, `utf8`, `markdown`, and `pdf`.
Stderr from mandoc is redirected to `/dev/null` to prevent error messages
from appearing in response bodies.

### Packages (`src/modules/packages/`)

Provides the `/api/packages/...` JSON namespace and the `/packages` view.
Wraps `pkg_info(1)` to implement search, info, which-file, file-list, and
installed-list queries. All output is captured through `safe_popen_read_argv`
with timeout and size limits.

---

## Storage layer

**`src/storage/`** contains stub implementations of a planned SQLite3 service
layer. The interfaces are defined but the SQLite3 backend is not yet wired:

- `mw_db_open()` / `mw_db_close()` — database lifecycle.
- `mw_db_exec_schema()` — schema initialisation.
- `mw_db_migrate()`, `mw_tx_begin()`, `mw_tx_commit()`, `mw_tx_rollback()` —
  schema migration and transaction control.
- `mw_stmt_prepare()`, `mw_bind_text()`, `mw_bind_int64()`,
  `mw_bind_null()`, `mw_stmt_step()`, `mw_stmt_finalize()` — prepared
  statement lifecycle.

All stub functions currently return `-1` (unimplemented). The headers are
stable and the implementations are intended to be filled in during the Phase 5
storage refactor.

---

## OpenBSD security hardening

`apply_openbsd_security()` is called after worker threads are started and
before the main event loop begins:

**`unveil(2)`** restricts filesystem visibility to the minimum required:

- `templates_dir` — read (template files)
- `static_dir` — read/write/create (static assets)
- `/usr/share/man`, `/usr/local/man`, `/usr/X11R6/man` — read (man pages)
- `mandoc_path`, `/usr/bin/man`, `/usr/bin/apropos` — execute
- `/usr/bin/netstat`, `/bin/sh` — execute (networking and package queries)
- `/etc/man.conf` — read
- `/dev/null` — read/write
- `/usr/sbin/pkg_info` — execute
- `/var/db/pkg`, `/usr/local`, `/usr/bin`, `/usr/sbin`, `/bin`, `/sbin`,
  `/usr/local/bin` — read (package database and `-E` lookups)
- `/etc/passwd`, `/etc/group`, `/etc/resolv.conf` — read

**`pledge(2)`** restricts allowed syscall categories to:
`stdio rpath wpath cpath inet route proc exec vminfo ps getpw`

On non-OpenBSD platforms both calls are compiled out; a debug message is
logged instead.

---

## Logging

**`src/core/log.c`** provides a thread-safe, mutex-protected logger:

- `log_init(path, verbose)` opens a log file (appending) or defaults to
  stderr if `path` is empty or `NULL`.
- `log_close()` flushes and closes the log file.
- `log_set_verbose(v)` updates the verbosity flag at runtime.
- `log_info`, `log_error` — always emitted.
- `log_debug` — emitted only when verbose mode is active.
- `log_errno(context)` — calls `log_error` with `strerror(errno)`.

All log lines are prefixed with a local-time timestamp and a level tag:
`[YYYY-MM-DD HH:MM:SS] [LEVEL] message`.

---

## Endpoints reference

### Views (HTML, template-rendered)

| Method | Path | Title |
|---|---|---|
| GET | `/` | Dashboard |
| GET | `/docs` | Documentation / man browser |
| GET | `/networking` | Networking |
| GET | `/packages` | Package Manager |
| GET | `/apiroot` | API Index |

### JSON APIs

| Method | Path | Description |
|---|---|---|
| GET | `/api/metrics` | System metrics snapshot (CPU, RAM, load, disk, processes) |
| GET | `/api/networking` | Networking diagnostics (routes, DNS, interfaces, connections) |
| GET | `/api/man/*` | Manual page discovery and search |
| GET | `/api/packages/search` | Search installed packages |
| GET | `/api/packages/info` | Package details |
| GET | `/api/packages/which` | Which package owns a file |
| GET | `/api/packages/files` | Files installed by a package |
| GET | `/api/packages/list` | List all installed packages |

### Rendered manual pages

| Method | Path | Description |
|---|---|---|
| GET | `/man/{area}/{section}/{page}` | Default HTML rendering |
| GET | `/man/{area}/{section}/{page}.html` | HTML |
| GET | `/man/{area}/{section}/{page}.utf8` | Plain UTF-8 text |
| GET | `/man/{area}/{section}/{page}.md` | Markdown |
| GET | `/man/{area}/{section}/{page}.pdf` | PDF |

### Static assets

| Method | Path | Description |
|---|---|---|
| GET | `/static/*` | CSS, JS, images, and other assets |
| GET | `/favicon.ico` | Favicon (served from `static/assets/favicon.svg`) |

---

## Source layout

```
src/
  app_main.c                      Main: kqueue loop, worker pool, accept, shutdown
  core/
    conf.c                        Configuration file parser and CLI override
    heartbeat.c                   Periodic task scheduler
    log.c                         Thread-safe logger
  http/
    response.c                    Response pool, serialisation, file cache, send helpers
    utils.c                       JSON escaping, subprocess execution (fork/poll/timeout)
  router/
    module_attach.c               Module attach/detach API
    route_table.c                 Static and hot-view cache, static file handler
    router.c                      router_register() wrapper
    url_registry.c                Route table, view routes, init_routes()
  modules/
    man/man_module.c              Man page rendering and JSON API
    metrics/metrics_module.c      System metrics collection and JSON API
    networking/networking_module.c Networking diagnostics and JSON API
    packages/packages_module.c    pkg_info(1) wrapper and JSON API
  render/
    template_render.c             Template cache, placeholder substitution
  storage/
    sqlite_db.c                   SQLite3 database lifecycle stubs
    sqlite_schema.c               Schema migration and transaction stubs
    sqlite_stmt.c                 Prepared statement stubs
include/
  conf.h                          miniweb_conf_t definition and parser API
  config.h                        Global config_verbose, config_static_dir, config_templates_dir
  heartbeat.h                     hb_task struct and scheduler API
  http_handler.h                  http_request_t, http_response_t, handler typedefs
  http_utils.h                    safe_popen_read_argv, json_escape_string
  log.h                           Logger API
  man.h                           Man module public API
  metrics.h                       Metrics structs and collection API
  networking.h                    Networking structs and collection API
  pkg_manager.h                   Package manager API
  routes.h                        route_handler_t, init_routes, route_match
  template_engine.h               template_data, render API
  urls.h                          route/view_route structs, MAX_ROUTES, registry API
  miniweb/core/heartbeat.h        Canonical heartbeat header
  miniweb/router/module_attach.h  miniweb_module struct and attach API
  miniweb/router/router.h         router struct and register API
  miniweb/storage/sqlite_db.h     mw_db API
  miniweb/storage/sqlite_schema.h Migration and transaction API
  miniweb/storage/sqlite_stmt.h   Statement binding and step API
```

---

## Enterprise refactor roadmap

| Phase | Description | Status |
|---|---|---|
| 0 | Test freeze / safety net | Planned |
| 1 | Module attach API + router separation | In progress (module-driven route attachment enabled) |
| 2 | Single global heartbeat scheduler | API ready; metrics/networking migration pending completion |
| 3 | Domain decomposition (metrics, networking, man, packages) | Ongoing |
| 4 | Documentation and architecture transition (docs/miniweb.1 + README.md, port modules to enterprise paradigm) | In progress |
| 5 | SQLite3 storage integration | Stub interfaces defined |
| 6 | Performance and observability hardening | Planned |

---

## Development standards

- Language: **C99 only**
- Style: **OpenBSD KNF**
- Documentation: **Doxygen** for all exported APIs
- Principle: small reusable functions, strict module ownership, minimal coupling
- TLS: not implemented — run behind `relayd(8)` for TLS termination
