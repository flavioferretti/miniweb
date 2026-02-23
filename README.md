# MiniWeb — OpenBSD C99 Web Framework (Enterprise Refactor Track)

[![OpenBSD](https://img.shields.io/badge/OpenBSD-7.8-orange.svg)](https://www.openbsd.org/)
[![License](https://img.shields.io/badge/license-BSD%203--Clause-blue.svg)](LICENSE)
[![C](https://img.shields.io/badge/language-C99-brightgreen.svg)](https://en.wikipedia.org/wiki/C99)

## What MiniWeb is now

MiniWeb is a lightweight OpenBSD-native HTTP server written in C99.
It currently ships with:

- system metrics dashboard,
- manual page browser,
- package manager endpoints,
- networking endpoints,
- template-driven web views,
- static file serving,
- kqueue + worker-thread runtime.

## What MiniWeb is becoming

MiniWeb is being re-engineered into an enterprise-grade general-purpose web framework with:

- clear modular boundaries,
- attachable/disable-able modules and routes,
- centralized heartbeat scheduling for expensive sampling,
- reusable sqlite3 storage facilities,
- strict C99 + OpenBSD KNF engineering discipline.

The canonical refactor blueprint is documented in:

- `docs/ENTERPRISE_REFACTOR_PLAN.md`

---

## Runtime model (current implementation)

- **dispatcher thread**: owns `kqueue(2)` and accept loop,
- **worker pool**: dequeues connections, parses HTTP, dispatches routes,
- **cache layers**: template cache, hot view cache, static file cache, metrics snapshot cache, networking sample ring,
- **security hardening**: `pledge(2)` and `unveil(2)`.

MiniWeb intentionally does not implement TLS directly. Run behind `relayd(8)` for TLS termination.

---

## Enterprise refactor principles

The refactor applies a strict **divide et impera** policy:

1. one responsibility per module,
2. one ownership location per exported function,
3. one centralized heartbeat for periodic heavy tasks,
4. one attach API for route/module composition,
5. one reusable storage layer for sqlite3-backed persistence.

Target layered architecture:

- `core/` lifecycle, heartbeat, config, logging,
- `net/` kqueue/server/connection-workflow,
- `http/` request+response+static+mimetype,
- `router/` route matching and module registration,
- `modules/` metrics/networking/man/packages/views,
- `platform/openbsd/` sysctl and OS collectors,
- `render/` template cache/rendering,
- `storage/` sqlite db/schema/statement helpers.

---


## Source layout (implemented after phase-1/phase-2 groundwork)

Current `src/` organization is now capability-oriented:

```text
src/
  app_main.c
  core/{config.c,heartbeat.c,log.c}
  http/{response.c,utils.c}
  router/{module_attach.c,route_table.c,router.c,url_registry.c}
  modules/
    man/man_module.c
    metrics/metrics_module.c
    networking/networking_module.c
    packages/packages_module.c
  render/template_render.c
  storage/{sqlite_db.c,sqlite_schema.c,sqlite_stmt.c}
```

This keeps URL behavior stable while moving feature implementation toward the
enterprise module boundaries described in the refactor plan.

---

## Feature status matrix

| Area | Current state | Enterprise target |
|---|---|---|
| Routing | Static table + dynamic prefixes | Module-owned route attach registry |
| Metrics scheduling | Feature-owned sampler loop | Global 1s heartbeat task scheduler |
| Networking scheduling | Feature-owned sampler loop | Global heartbeat-managed task |
| Storage | In-memory + filesystem caches | Reusable sqlite3 service layer |
| File organization | Several large source files | Capability-based modular tree |
| API extensibility | Route edits in central code | Plug-in module attach/detach |

---

## Build and run

### Prerequisites

- OpenBSD 7.8+
- base tools: `clang`, `make`
- optional: `wrk`, `gnuplot`

### Build

```sh
make clean && make
```

### Build manual page

```sh
make man
```

### Unit tests

```sh
make unit-tests
```

### Run

```sh
./build/miniweb -v
```

Open: <http://127.0.0.1:9001>

---

## CLI

```text
./build/miniweb [options]

  -f FILE   Config file path (default: auto-detect)
  -p PORT   Listen port (default: 9001)
  -b ADDR   Bind address (default: 127.0.0.1)
  -t NUM    Worker threads (default: 4, max 32 by compile-time limit)
  -c NUM    Max concurrent connections (default: 1280)
  -l FILE   Log file path (default: stderr)
  -v        Verbose logging
  -h        Help
```

---

## Endpoints (current)

### Views

- `GET /`
- `GET /docs`
- `GET /networking`
- `GET /packages`
- `GET /apiroot`

### APIs

- `GET /api/metrics`
- `GET /api/networking`
- `GET /api/man/*`
- `GET /api/packages/*`

### Man rendering

- `GET /man/{area}/{section}/{page}[.fmt]`

### Static

- `GET /static/*`
- `GET /favicon.ico`

---

## Refactor execution roadmap

1. **Phase 0:** test freeze/safety net.
2. **Phase 1:** extract core infra from monolithic files.
3. **Phase 2:** module attach API + router separation.
4. **Phase 3:** single heartbeat scheduler.
5. **Phase 4:** domain decomposition (`metrics`, `networking`, `man`, `packages`).
6. **Phase 5:** sqlite3 storage integration.
7. **Phase 6:** performance and observability hardening.

For detailed tasks, read:

- `docs/ENTERPRISE_REFACTOR_PLAN.md`

---


## Documentation alignment

- `README.md` and `docs/miniweb.1` now track the enterprise refactor program vocabulary and lifecycle.
- Runtime behavior descriptions remain explicit about current implementation vs planned architecture phases.

---
## Development standards

- Language: **C99 only**
- Style: **OpenBSD KNF**
- Documentation: **Doxygen per exported API**
- Direction: small reusable functions, strict module ownership, minimal coupling

