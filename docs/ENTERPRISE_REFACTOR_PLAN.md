# MiniWeb Enterprise Refactor Plan (C99 + OpenBSD KNF)

## Refactor status snapshot (verified against repository on 2026-02-24)

This plan is **not totally outdated**. The high-level direction is still valid, but
many items from the previous version were already completed and several file-level
observations were stale.

---

## 1) Reality check: plan vs current codebase

### 1.1 Completed since the original draft

- Capability-oriented source layout is already in place in `src/`:
  `core`, `http`, `router`, `modules`, `render`, `storage`, plus `app_main.c` entrypoint.
- Namespaced headers are in `include/miniweb/` for core/http/router/modules/render/storage.
- Heartbeat scheduler exists and is feature-complete for periodic registration and lifecycle:
  `heartbeat_init/register/unregister/update/start/stop`.
- Router/module attach boundary is active with `struct miniweb_module` and
  `miniweb_module_attach_enabled()`.
- SQLite facade layer exists with `sqlite_db`, `sqlite_stmt`, and `sqlite_schema` units.
- Route-level tests already assert 404/405 semantics at the matcher/API level
  (`route_path_known`, `route_allow_methods`).

### 1.2 Still true / still risky

- Very large files remain and should be decomposed further:
  - `src/http/response.c` (~973 LOC)
  - `src/modules/metrics/metrics_module.c` (~1432 LOC)
  - `src/modules/man/man_module.c` (~977 LOC)
- `src/app_main.c` was reduced and partially extracted (~904 LOC), but still
  needs follow-up splits (`server`, `worker`, `connection_pool`).
- Feature files still combine service logic, JSON serialization, and route handlers
  in the same translation unit for several modules.
- The platform wrapper layer (`platform/openbsd/*`) and dedicated `net/` folder from the
  target architecture are not yet implemented.
- Integration checks exist but are still smoke-oriented (endpoint reachability),
  not full golden-payload assertions.

### 1.3 Outdated statements from previous version (corrected)

- References to `src/main.c`, `src/metrics.c`, and `src/http_handler.c` are obsolete.
  Current code already moved those responsibilities into `app_main.c`,
  `src/modules/metrics/metrics_module.c`, and `src/http/response.c`.
- The roadmap item “introduce module attach API” is no longer pending; it is implemented.
- The roadmap item “introduce sqlite facade” is no longer pending; scaffold is implemented.

---

## 2) Updated target architecture and gap map

### 2.1 Target layers (kept)

1. `core/` — lifecycle, heartbeat, config, logging, threading primitives.
2. `net/` — accept loop, connection/work queues, transport I/O.
3. `http/` — parser/response writer/static assets/mime helpers.
4. `router/` — registry, matching, module route-attach integration.
5. `modules/` — feature packages (`metrics`, `networking`, `man`, `packages`, views/api root).
6. `storage/` — sqlite facade and schema migration helpers.
7. `platform/openbsd/` — collectors for sysctl/kvm/getifaddrs/getmntinfo.
8. `render/` — template render/cache/composition.

### 2.2 Current implementation status by layer

- `core/` ✅ present
- `http/` ✅ present
- `router/` ✅ present
- `modules/` ✅ present
- `storage/` ✅ present
- `render/` ✅ present
- `net/` 🟡 partially extracted (`work_queue.c` extracted; server/worker still in `app_main.c`)
- `platform/openbsd/` 🟡 partially extracted (`security.c` extracted; collectors still embedded in modules)

---

## 3) Execution plan (re-prioritized)

### Phase A — Decompose highest-risk files (next sprint)

1. Split `src/app_main.c` into:
   - `src/net/server.c` (listen/accept/kqueue loop)
   - `src/net/connection_pool.c`
   - `src/net/work_queue.c`
   - `src/net/worker.c`
2. Split `src/http/response.c` into:
   - `response_writer.c`
   - `static_files.c`
   - `mime.c` (if still embedded)
3. Split each large feature module into `*_service.c`, `*_json.c`, `*_module.c`:
   - metrics
   - networking
   - man
   - packages

**Definition of done (Phase A):**
- No file above 700 LOC.
- Public behavior unchanged (unit + integration tests pass).

### Phase B — Platform boundary and reusable collectors

1. Introduce `src/platform/openbsd/` collectors for CPU/memory/disk/process/net/uptime.
2. Make modules consume collector APIs only (no direct sysctl/kvm/getifaddrs in modules).
3. Add minimal tests (or deterministic stubs) per collector-facing adapter.

**Definition of done (Phase B):**
- Modules call platform wrappers only.
- Collector code can be mocked in tests.

### Phase C — Test hardening and payload contracts

1. Expand `tests/integration_endpoints.sh` into contract-style checks:
   - explicit status assertions (200/404/405)
   - JSON structure checks for core endpoints
2. Add golden payload fixtures for at least:
   - `/api/metrics`
   - `/api/networking`
   - `/api/packages/list` (or equivalent stable endpoint)
3. Add CI gate to run unit tests + integration tests on each change.

**Definition of done (Phase C):**
- Endpoint payload regressions detected automatically.
- Route semantics verified both at matcher and HTTP response levels.

### Phase D — SQLite adoption beyond scaffold

1. Persist module enable/disable flags.
2. Persist selected sampled snapshots (bounded retention).
3. Version schemas through explicit migration list.

**Definition of done (Phase D):**
- Restart preserves selected runtime state.
- Migration path is deterministic and test-covered.

---

## 4) Coding governance (kept and clarified)

- C99 + OpenBSD KNF style is mandatory.
- File-size guideline:
  - soft limit 350 LOC
  - review trigger 500 LOC
- Function-size guideline:
  - soft limit 40 LOC
  - review trigger 80 LOC
- Return convention for internal APIs: `0` success, `-1` failure unless documented otherwise.
- Every exported symbol must have:
  - one owning header
  - one meaningful Doxygen block
  - one test entry when practical

---

## 5) Immediate actionable next steps (ordered backlog)

1. Complete extraction of connection-pool/server/worker units from `app_main.c` into `src/net/`.
2. Carve HTTP static serving + response writing out of `src/http/response.c`.
3. Start with `metrics` module split (`service/json/module`) as the reference pattern.
4. Upgrade integration test script to assert 404/405 and basic JSON key presence.
5. Remove stale TODO Doxygen placeholders while touching each file.

This document should now be treated as the active enterprise roadmap baseline.

## 2026-02-24 Incremental extraction update

Completed in this slice:
- `app_main.c` trimmed to orchestration/bootstrap only.
- New net runtime units introduced:
  - `src/net/server.c` (listen + accept + kqueue dispatch loop)
  - `src/net/connection_pool.c` (fd-indexed pool with generation guard)
  - `src/net/worker.c` (request read/parse/dispatch lifecycle)
- New module decomposition stubs introduced for enterprise layering:
  - `metrics_service.c`, `metrics_json.c`
  - `networking_service.c`, `networking_json.c`
  - `man_service.c`, `man_json.c`
  - `packages_service.c`, `packages_json.c`
- Build graph updated to compile all newly extracted translation units.

Outstanding follow-ups in next slices:
- Finish splitting `src/http/response.c` into `response_writer.c`, `static_files.c`, and optional `mime.c`.
- Move function bodies from module `*_module.c` into the new `*_service.c` and `*_json.c` units (currently compatibility scaffolds).
- Extract `work_queue` abstraction details into dedicated private header for stricter net-layer encapsulation.
