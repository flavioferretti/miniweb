# MiniWeb Enterprise Refactor Plan (C99 + OpenBSD KNF)

## 1) In-depth codebase review

### 1.1 High-level findings

MiniWeb already has strong low-level primitives (kqueue dispatching, worker queue,
multiple caches, OpenBSD-native data collection), but architecture is currently
**feature-oriented by file growth** instead of **capability-oriented by module boundary**.

The largest maintainability risks are:

1. **God files** with multiple responsibilities.
   - `src/main.c` (~964 LOC) mixes lifecycle, socket bootstrapping, pool allocation,
     worker coordination, signal handling, and config wiring.
   - `src/metrics.c` (~1385 LOC) mixes kernel sampling, ring storage, snapshot caching,
     JSON assembly, and HTTP endpoint coupling.
   - `src/http_handler.c` (~973 LOC) mixes response pooling, static file serving,
     MIME responsibilities, write/retry policy, and file cache internals.
2. **Routing concerns split across multiple locations** (`routes.c` and `urls.c`) with
   partially duplicated path/method logic.
3. **Domain logic coupled to transport/HTTP** (e.g., metrics functions living in files
   that also build HTTP payload responses).
4. **Multiple 1-second sampling loops** (metrics and networking) instead of a single
   global heartbeat scheduler.
5. **No generalized service/plugin contract** for enabling/disabling view/json routes as
   independent modules.
6. **No generic data-access layer** despite natural fit for a reusable sqlite3-backed
   persistence module for state/history/configuration.

### 1.2 Current architecture observations (from code)

- Runtime model is a kqueue dispatcher + worker pool, a good base for scale.
- A metrics ring sampler already runs every second in a detached thread.
- A networking sampler thread also runs every second.
- Route matching currently supports exact table routes plus dynamic prefix routes.
- Caching exists at multiple layers (templates, hot view cache, static cache,
  metrics snapshot, networking ring, package query cache).

These are strong building blocks for enterprise architecture if boundaries are formalized.

### 1.3 Root causes of growing complexity

- **File-centric accretion**: new features were added to existing large files instead of
  extracting focused modules.
- **API surface not explicitly layered**: transport layer can call deep internals directly.
- **No per-function placement policy**: helper functions remain in their first birthplace.
- **No central scheduler abstraction**: periodic tasks each spawn their own loop.

---

## 2) Target architecture (divide et impera)

Adopt a strict layer model:

1. **core/**
   - process lifecycle, heartbeat scheduler, config, logging, thread primitives.
2. **net/**
   - socket/kqueue accept loop, connection/work queues, protocol IO.
3. **http/**
   - request parsing, response writer, static file serving, MIME table, middleware.
4. **router/**
   - route registry, method/path matchers, module route attach API.
5. **modules/**
   - feature modules: dashboard, docs/man, metrics, networking, packages, api_root.
6. **storage/**
   - sqlite3 facade and schema/bootstrap helpers.
7. **platform/openbsd/**
   - sysctl/kvm/getifaddrs/getmntinfo wrappers; pure data collection API.
8. **render/**
   - template engine and view composition.

### Proposed filesystem redesign

```text
include/miniweb/
  core/{app.h,config.h,heartbeat.h,log.h,errors.h}
  net/{server.h,connection_pool.h,work_queue.h}
  http/{request.h,response.h,static_files.h,mime.h}
  router/{router.h,route_table.h,module_attach.h}
  modules/
    metrics/{metrics_module.h,metrics_service.h,metrics_json.h}
    networking/{networking_module.h,networking_service.h,networking_json.h}
    man/{man_module.h,man_service.h,man_json.h}
    packages/{packages_module.h,packages_service.h,packages_json.h}
    views/{views_module.h,view_registry.h}
  render/{template_cache.h,template_render.h}
  storage/{sqlite_db.h,sqlite_schema.h,sqlite_stmt.h}
  platform/openbsd/{cpu.h,memory.h,disk.h,proc.h,netif.h,routes.h,uptime.h}

src/
  core/*.c
  net/*.c
  http/*.c
  router/*.c
  modules/**.c
  render/*.c
  storage/*.c
  platform/openbsd/*.c
  app_main.c
```

Filename intent should be self-evident: if a function concerns process listing,
it belongs in `platform/openbsd/proc.c`; if it transforms model to JSON, it belongs
in `modules/*/*_json.c`; if it exposes endpoint wiring, `*_module.c`.

---

## 3) Single 1-second heartbeat architecture

## Goal

One global heartbeat thread dispatches timed jobs (1s/5s/30s/60s), replacing
feature-owned infinite loops.

### API sketch (C99, KNF-friendly)

- `heartbeat_init(void)`
- `heartbeat_register(const struct hb_task *task)`
- `heartbeat_start(void)`
- `heartbeat_stop(void)`

Where `hb_task` contains:

- task name
- period in seconds
- initial delay
- callback pointer
- opaque context pointer
- optional failure counter/backoff policy

### Usage

- Metrics sampler task registers at 1s.
- Networking sampler task registers at 1s.
- Template cache refresh or cleanup at 30s.
- Static cache compaction at 60s.

### Benefits

- One control plane for expensive sampling.
- Consistent timing and observability.
- Easier future throttling and backpressure.
- Clean shutdown ordering and thread accountability.

---

## 4) Route enable/disable and module attach API

Create a lightweight module contract:

```c
struct miniweb_module {
	const char *name;
	int (*init)(void *ctx);
	int (*attach_routes)(struct router *r);
	void (*shutdown)(void *ctx);
	int enabled_by_default;
};
```

At startup:

1. Load module registry.
2. Apply config-based enable/disable flags.
3. For each enabled module: `init` then `attach_routes`.

This makes views/json endpoints togglable without touching central route logic.

Config examples:

- `module.metrics=on`
- `module.packages=off`
- `module.views.docs=on`
- `module.api.man=off`

---

## 5) sqlite3 reusable interface library

Provide a `storage/sqlite` layer designed as boilerplate facilities for modules.

### Minimal API

- DB lifecycle:
  - `mw_db_open(path, flags, &db)`
  - `mw_db_close(db)`
- Schema bootstrap:
  - `mw_db_exec_schema(db, schema_sql)`
  - `mw_db_migrate(db, migrations[], n)`
- CRUD helpers:
  - `mw_stmt_prepare(db, sql, &stmt)`
  - bind helpers (`mw_bind_text/int64/blob/null`)
  - `mw_stmt_step(stmt)`
  - `mw_stmt_finalize(stmt)`
- Transaction helpers:
  - `mw_tx_begin/commit/rollback`

### First practical use-cases

1. Persist route/module flags (runtime configuration).
2. Persist sampled heartbeat snapshots (rolling history).
3. Persist package/man query caches for warm restart.

Keep all SQL in dedicated `*_schema.c` files with explicit versioning.

---

## 6) Per-function separation policy

Adopt explicit placement rules:

1. **Collector function** (sysctl/getifaddrs/etc.) -> `platform/openbsd/*`.
2. **Model aggregation function** -> `modules/*/*_service.c`.
3. **Serialization function** -> `modules/*/*_json.c`.
4. **HTTP handler** -> `modules/*/*_module.c`.
5. **Router binding** -> module `attach_routes` only.

Each exported function must have:

- One Doxygen comment block (`@brief`, params, return, thread safety note if needed).
- One owner header.
- One unit test entry when practical.

---

## 7) C99 + OpenBSD KNF compliance intervention list

1. Keep declarations at block starts when possible and avoid mixed declaration style
   that hurts readability.
2. Use KNF indentation/tabs, line wrapping, and brace placement consistently.
3. Normalize return conventions (`0` success, `-1` failure) across internal APIs.
4. Replace ad-hoc magic numbers with named `enum` or `#define` in owning module.
5. Limit file size target:
   - soft limit 350 LOC
   - hard review trigger 500 LOC
6. Limit function size target:
   - soft limit 40 LOC
   - hard review trigger 80 LOC

---

## 8) Refactor execution roadmap (phased)

### Phase 0 — Safety net (1-2 weeks)

- Freeze behavior with integration tests for all current endpoints.
- Add route conformance tests (405/404 semantics).
- Add golden payload smoke tests for `/api/metrics` and `/api/networking`.

### Phase 1 — Extract core infrastructure (1-2 weeks)

- Extract from `main.c`:
  - connection pool
  - work queue
  - worker lifecycle
  - accept loop helpers
- Keep external behavior unchanged.

### Phase 2 — Router and module boundary (1 week)

- Introduce router object and module attach API.
- Migrate existing routes into modules without changing URL contracts.

### Phase 3 — Heartbeat unification (1 week)

- Add `core/heartbeat`.
- Move metrics/networking samplers to heartbeat tasks.
- Remove duplicated sampler threads.

### Phase 4 — Domain decomposition (2-3 weeks)

- Split `metrics.c` into:
  - openbsd collectors
  - metrics service
  - json serializer
  - endpoint module
- Apply same pattern to networking/packages/man.

### Phase 5 — sqlite3 storage layer (1-2 weeks)

- Add storage library and schema bootstrap.
- Migrate one cache/state feature first (feature flag table).
- Expand usage gradually after proving stability.

### Phase 6 — performance hardening (ongoing)

- Instrument heartbeat tasks with runtime counters.
- Add watchdog logs for slow callbacks.
- Tune cache TTL/token budgets based on measured latency.

---

## 9) Suggested immediate backlog (next sprint)

1. Create architecture decision records (ADR-001: layering, ADR-002: heartbeat,
   ADR-003: module API).
2. Extract `connection_pool.*` and `work_queue.*` from `main.c` first (lowest risk).
3. Introduce `router/router.c` object while preserving existing `init_routes()` API as shim.
4. Define `modules/metrics` folder and move only HTTP glue first.
5. Add sqlite3 wrapper skeleton with no runtime dependency yet.

---

## 10) Success criteria

You can declare miniweb "enterprise-ready" when:

- Every endpoint belongs to a module with independent enable/disable flag.
- No core source file exceeds 500 LOC.
- Expensive collectors are triggered only by centralized heartbeat tasks.
- Platform collectors are transport-agnostic (usable by CLI/tests/http).
- sqlite3 storage layer is reused by at least two modules.
- C99 + KNF checks pass in CI and documentation is Doxygen-complete.



---

## 11) Migration status update (current repository snapshot)

### Completed in this stage

1. **Module attach contract is active** via `router` abstraction and `miniweb_module` wiring.
2. **Heartbeat scheduler abstraction exists** and is used as the single periodic task control plane.
3. **SQLite storage facade scaffolding exists** under `src/storage` and `include/miniweb/storage`.
4. **Source tree is now capability-oriented** (`core`, `http`, `router`, `modules`, `render`, `storage`).

### Remaining gaps before enterprise completion

1. Move remaining legacy includes to `include/miniweb/**` only and remove compatibility headers.
2. Complete SQLite backend implementation (`sqlite3_open_v2`, prepared statement execution, error mapping).
3. Add module-level enable/disable flags loaded from config and persisted in SQLite.
4. Add integration tests for module toggles and heartbeat task lifecycle semantics.
5. Finish Doxygen coverage and enforce via CI gate.

### Code review (focus areas)

- **Documentation quality**: comments are improving but many legacy functions still use placeholder descriptions.
- **Storage behavior**: `src/storage/*.c` are currently stubs; API shape is good, runtime behavior is incomplete.
- **Thread lifecycle**: heartbeat thread currently uses detach semantics; consider join-based shutdown for deterministic teardown.
- **Header organization**: enterprise headers under `include/miniweb/` are present; continue migrating all includes to this namespace.
- **Performance observability**: add per-task heartbeat latency counters and expose them in metrics JSON.
