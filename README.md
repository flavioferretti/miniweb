# MiniWeb — OpenBSD System Monitoring Server

[![OpenBSD](https://img.shields.io/badge/OpenBSD-7.8-orange.svg)](https://www.openbsd.org/)
[![License](https://img.shields.io/badge/license-BSD%203--Clause-blue.svg)](LICENSE)
[![C](https://img.shields.io/badge/language-C99-brightgreen.svg)](https://en.wikipedia.org/wiki/C99)

A lightweight HTTP server written in C99 for OpenBSD. Provides a system monitoring dashboard, RESTful JSON API, and an integrated manual page browser. No external library dependencies — built entirely on native OpenBSD interfaces.

![Dashboard Screenshot](docs/screenshot.png)

---

## Features

- **Real-time System Metrics** — CPU, memory, swap, load averages, disk, network interfaces
- **Process Monitoring** — top processes by CPU and memory with live updates
- **Man Page Browser** — search and render OpenBSD manual pages in HTML, plain text, PDF, or Markdown
- **Security Hardened** — `pledge(2)` and `unveil(2)` sandboxing from startup
- **High Performance** — kqueue dispatcher + worker thread pool, up to 34,771 req/s on static content (OpenBSD 7.8, 4 cores)
- **RESTful JSON API** — clean endpoints for integration with external monitoring tools
- **No TLS, by design** — intended to run behind `relayd(8)` for TLS termination
---


## Quick Start

### Prerequisites

- OpenBSD 7.8 or later
- Standard development tools (`clang`, `make`) — included in base
- `wrk` for benchmarking: `pkg_add wrk` *(optional)*
- `gnuplot` for benchmark graphs: `pkg_add gnuplot` *(optional)*

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

Open `http://127.0.0.1:9001` in your browser.

---

## Usage

```
./build/miniweb [options]

  -f FILE   Config file path (default: auto-detect)
  -p PORT   Port to listen on        (default: 9001)
  -b ADDR   Address to bind to       (default: 127.0.0.1)
  -t NUM    Worker thread count       (default: 4)
  -c NUM    Max concurrent conns      (default: 1280)
  -v        Verbose logging to stderr
  -h        Show this help
```

### Examples

```sh
# Start with verbose logging on the default port
./build/miniweb -v

# Load an explicit config file
./build/miniweb -f /etc/miniweb.conf

# Override a single value from the config file on the CLI
./build/miniweb -f /etc/miniweb.conf -p 8080

# Listen on all interfaces, 8 workers
./build/miniweb -b 0.0.0.0 -t 8

# Production: loopback only, high connection limit
./build/miniweb -b 127.0.0.1 -c 1280 -t 4
```

---

## Configuration File

`miniweb` loads a configuration file at startup. Search order (first match wins):

1. `-f /path/to/file` — explicit path from CLI
2. `./miniweb.conf` — current working directory
3. `$HOME/.miniweb.conf`
4. `/etc/miniweb.conf`

CLI flags always override file values. If no file is found, compiled-in defaults are used.

### Format

```
# comment
key    value
```

One directive per line, keys case-insensitive, unknown keys produce a warning but do not abort startup.

### All directives

| Key | Default | Description |
|---|---|---|
| `port` | `9001` | TCP port |
| `bind` | `127.0.0.1` | IPv4 bind address |
| `threads` | `4` | Worker thread count (max: `THREAD_POOL_SIZE`) |
| `max_conns` | `1280` | Max concurrent connections |
| `conn_timeout` | `30` | Idle connection timeout (seconds) |
| `max_req_size` | `16384` | Max request size (bytes) |
| `mandoc_timeout` | `10` | `mandoc(1)` subprocess timeout (seconds) |
| `static_dir` | `static` | Static assets directory |
| `templates_dir` | `templates` | HTML templates directory |
| `mandoc_path` | `/usr/bin/mandoc` | Path to `mandoc(1)` binary |
| `trusted_proxy` | `127.0.0.1` | IP from which `X-Forwarded-*` headers are trusted |
| `verbose` | `no` | Verbose logging (`yes`/`no`/`true`/`false`/`1`/`0`) |

A fully commented example is provided in `docs/miniweb.conf` at the project root.

### `/etc/rc.d/miniweb`

The natural way to point `rcctl` at a config file:

```sh
# /etc/rc.d/miniweb
daemon="/usr/local/bin/miniweb"
daemon_flags="-f /etc/miniweb.conf"
. /etc/rc.d/rc.subr
rc_cmd $1
```

```sh
rcctl enable miniweb
rcctl start miniweb
```

---

### Web Interface

| Endpoint | Description |
|---|---|
| `GET /` | Dashboard — load, memory, disk, network, processes |
| `GET /docs` | Manual page browser with `apropos(1)` search |
| `GET /networking` | Network interfaces, routes, DNS |
| `GET /apiroot` | API index |

### Add or Remove Regular Pages (`/`, `/docs`, ...)

Template-backed pages are declared in one place: `view_routes[]` in `src/urls.c`.
Each entry maps a URL path to a template and optional extra partials used by `base.html`.

#### Add a new page (example: `/about`)

1. Create page templates under `templates/`:
   - `about.html` (main page body)
   - `about_extra_head.html` (optional, can be empty)
   - `about_extra_js.html` (optional, can be empty)
2. Add a new `view_routes[]` entry in `src/urls.c`:

```c
{"GET", "/about", "MiniWeb - About", "about.html",
 "about_extra_head.html", "about_extra_js.html"},
```

3. Rebuild and run:

```sh
make clean && make
./build/miniweb -v
```

The route is registered automatically by `init_routes()` (same file), because all
entries in `view_routes[]` are bound to `view_template_handler`.

#### Remove an existing page

1. Remove (or comment) the corresponding entry from `view_routes[]` in `src/urls.c`.
2. Optionally delete the associated templates in `templates/`.
3. Rebuild.

After removal, requests to that path will no longer match and will return `404 Not Found`.

### Metrics API

| Endpoint | Response |
|---|---|
| `GET /api/metrics` | JSON — full system snapshot |
| `GET /api/networking` | JSON — interfaces, routes, resolver config |

`/api/metrics` includes: hostname, OS version, uptime, CPU load averages (1/5/15 min), memory (total/free/active/wired/cached), swap, per-filesystem disk usage, network interface status, top 10 processes by CPU, top 10 by memory, process counts (total/running/sleeping/zombie).

### Manual Pages API

| Endpoint | Description |
|---|---|
| `GET /api/man/search?q=query` | `apropos(1)` search, returns plain text (one entry per line) |
| `GET /man/{area}/{section}/{page}` | Render man page as HTML (default) |
| `GET /man/{area}/{section}/{page}.html` | HTML |
| `GET /man/{area}/{section}/{page}.md` | Markdown |
| `GET /man/{area}/{section}/{page}.pdf` | PDF |
| `GET /man/{area}/{section}/{page}.ps` | PostScript |

### Static Files

| Endpoint | Description |
|---|---|
| `GET /static/*` | CSS, JavaScript, images — path traversal rejected |
| `GET /favicon.ico` | SVG favicon |

---

## Architecture

### Project Structure

```
.
├── Makefile
├── README.md
├── benchmark.sh
├── build/
├── docs/
├── include/
│   ├── conf.h
│   ├── config.h
│   ├── http_handler.h
│   ├── http_utils.h
│   ├── man.h
│   ├── metrics.h
│   ├── networking.h
│   ├── routes.h
│   ├── template_engine.h
│   └── urls.h
├── src/
│   ├── conf.c
│   ├── http_handler.c
│   ├── http_utils.c
│   ├── main.c
│   ├── man.c
│   ├── metrics.c
│   ├── networking.c
│   ├── routes.c
│   ├── template_engine.c
│   └── urls.c
├── static/
├── templates/
└── tests/
    ├── integration_endpoints.sh
    ├── routes_test.c
    └── template_test.c
```

## Codebase diagram

![Diagram](docs/miniweb_diagram.svg)
---

### Request Lifecycle

```
accept()
   │
   ▼
kqueue dispatcher (main thread)
   │  EV_DISPATCH → event auto-disabled after delivery
   ▼
work queue  (pthread_cond_wait, zero busy-waiting)
   │
   ▼
worker thread (1 of N)
   │  recv() → parse → route_match() → handler()
   ▼
close(fd) + free_connection()
```

The main thread does nothing but `accept()` and enqueue. Workers never call `kevent()` — no races, no busy-waiting.

`EV_DISPATCH` is the key: kqueue auto-disables the event after the first delivery, so the same fd can never reach two workers simultaneously.

### Key Components

#### `main.c` — Dispatcher + Worker Pool

- Single `kqueue` instance, owned exclusively by the main thread
- `EV_ADD | EV_DISPATCH` on every client socket — auto-disables after first event
- Lock-free hot path: `queue_push()` signals workers via `pthread_cond_signal`
- Connection pool indexed directly by fd — O(1) alloc/free
- Generation counter per slot detects stale `udata` pointers from recycled fds
- Idle timeout sweep (30 s) runs in the main loop, once per second

#### `metrics.c` — System Metrics

Uses `sysctl(2)` directly — no external processes for metrics:

- Memory: `CTL_VM / VM_UVMEXP` for UVM statistics
- Load: `getloadavg(3)`
- Disks: `getmntinfo(3)`
- Processes: `KERN_PROC / KERN_PROC_ALL` with a retry loop for the TOCTOU window between size query and data fetch
- Network: `getifaddrs(3)`

All metric functions use thread-safe variants: `localtime_r()`, `getpwuid_r()`.

#### `http_handler.c` — Response Helpers

`http_request_get_header()` and `http_request_get_client_ip()` write into per-request scratch buffers inside `http_request_t`, making them safe to call from multiple worker threads without any locks.

Reverse-proxy header precedence for client IP: `X-Real-IP` → `X-Forwarded-For` (first entry) → socket peer address.

#### `template_engine.c` — Templates

Simple `{{TOKEN}}` substitution. Reads layout and page-content files from `templates/` at request time.

---

## Performance

Measured on OpenBSD 7.8, AMD64, 4-core CPU.

| Endpoint | Throughput | Avg latency |
|---|---|---|
| `/static/test.html` | **34,771.76 req/s (peak)** | **0.220 ms (best avg)** |
| `/api/metrics` | ~300 req/s | — |

Latest static-file benchmark report (`wrk`, 30s, 4 threads, target `http://localhost:3000/static/test.html`):

- Peak throughput: **34,771.76 req/s**
- Best average latency: **0.220 ms**
- Worst maximum latency: **118.010 ms**

Peak throughput versus the earlier `~7,000 req/s` baseline is approximately **+396.74%** (about **4.97×**).

Memory footprint at idle: ~8–12 MB resident. Under load with 4 workers: ~50–80% CPU across all cores.

---

## Security

### Sandboxing

`unveil(2)` limits filesystem access to exactly:

```
templates/          r
static/             r
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
/etc/passwd         r
/etc/group          r
/etc/resolv.conf    r
```

`pledge(2)` promise set:

```
stdio rpath inet route proc exec vminfo ps getpw
```

### Other mitigations

- Path traversal: any static path containing `..` or `//` gets a 403
- Connection limit: excess connections receive 503 and are closed immediately
- Subprocess sanitisation: page/section/area components in `/man/` URLs are run through `sanitize_string()` before being passed to `mandoc(1)`
- Reverse-proxy headers (`X-Forwarded-For`, `X-Forwarded-Proto`) are set by `relayd` and must not be accepted from direct external connections — bind to `127.0.0.1` in production

### Known limitations

- No TLS — deploy behind `relayd(8)` or another terminating proxy
- No authentication — add at the proxy layer
- JSON output uses fixed-size buffers; extremely long hostnames or mount point paths may be truncated
- Man page rendering spawns `mandoc(1)` as a subprocess; a kernel stall inside mandoc cannot be interrupted by the timeout mechanism

---

## Reverse Proxy with relayd

Minimal `/etc/relayd.conf` for HTTPS termination:

```
table <miniweb> { 127.0.0.1 }

http protocol "https_proxy" {
    tls

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

Provision a certificate with `acme-client(1)`, then:

```sh
rcctl enable relayd
rcctl start relayd
relayctl show summary
```

---

## Development

### Testing

```sh
# Unit tests
make unit-tests

# Manual endpoint check through the proxy
for p in / /docs /apiroot /networking \
          /api/metrics /api/networking \
          /static/css/custom.css /favicon.ico; do
    printf '%-30s %s\n' "$p" \
      "$(curl -s -o /dev/null -w '%{http_code}' http://127.0.0.1:9001$p)"
done

# Man page search
curl -s 'http://127.0.0.1:9001/api/man/search?q=pledge'

# Metrics
curl -s http://127.0.0.1:9001/api/metrics | jq .
```

### Debug build

```sh
make DEBUG=1
./build/miniweb -v
```

---

## Troubleshooting

**`bind: Address already in use`**  
Another process owns the port. Find it with `fstat | grep :9001`.

**`pledge: Operation not permitted`**  
A syscall was made outside the promise set. Required promises: `stdio rpath inet route proc exec vminfo ps getpw`. Check that none have been accidentally removed.

**`sysctl data query failed: Cannot allocate memory`**  
The process table grew between the size query and the data fetch. The built-in retry loop handles this automatically; if it persists there is unusual process churn on the system.

**`[write_all] Too many EAGAIN retries`**  
The client socket buffer stayed full for too long. The connection is dropped. Usually indicates a very slow or unresponsive client.

**Man pages not rendering**  
Verify `mandoc` is installed and that `/usr/bin/mandoc` is unveiled. Check verbose output with `-v`.

---

## Roadmap

- [ ] WebSocket support for live metric streaming
- [ ] Historical data with ring-buffer storage and simple graphs
- [ ] Alert thresholds and notifications
- [ ] IPv6 support
- [ ] Prometheus exporter endpoint
- [ ] Port to FreeBSD / NetBSD
- [x] Native kqueue engine (replaced libmicrohttpd)
- [x] EV_DISPATCH worker pool — eliminated busy-waiting, static-file throughput now measured up to 34,771.76 req/s (~+396.74% vs ~7,000 req/s baseline)
- [x] Configuration file (`miniweb.conf`) with full runtime knobs
- [x] Unit test suite
- [x] Thread-safe metric collection
- [x] KERN_PROC_ALL retry loop
- [x] relayd reverse proxy documentation and header handling

---

## License

BSD 3-Clause License — see [LICENSE](LICENSE).

---

## Acknowledgments

Built on OpenBSD's security-first philosophy and its excellent system interfaces: `kqueue(2)`, `pledge(2)`, `unveil(2)`, `sysctl(2)`.
