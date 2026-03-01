# MiniWeb refactor track (status refresh + modules extraction)

## Scope analyzed in `src/`

- `src/http/`: request/response marshalling, header parsing, static file path and subprocess helpers.
- `src/net/`: accept loop, worker handoff, queueing, kqueue lifecycle.
- `src/router/`: route lookup tables, URL registry, module attachment.
- `src/render/`: template expansion and page composition logic.
- `src/core/`: config loading, logging, heartbeat management.
- `src/modules/*`: feature modules (`metrics`, `man`, `networking`, `packages`) and JSON/service delegation.
- `src/storage/`: sqlite connection/schema/statement wrappers.
- `src/platform/openbsd/`: OpenBSD specific security hooks.

## What is already extracted (current tree)

The older “next steps” list is partially obsolete. Current split status:

- HTTP response path is split into:
  - `src/http/response_api.c`
  - `src/http/response_helpers.c`
  - `src/http/response_file.c`
  - `src/http/response_io.c`
  - `src/http/response_pool.c`
  - `src/http/response_file_cache.c`
  - shared private contract: `include/miniweb/http/response_internal.h`
- Router URL registry is split into:
  - `src/router/url_registry.c`
  - `src/router/url_registry_init.c`
  - `src/router/url_registry_lookup.c`
  - `src/router/url_registry_reverse.c`
  - shared private contract: `src/router/url_registry_internal.h`
- Core heartbeat is split into:
  - `src/core/heartbeat.c`
  - `src/core/heartbeat_schedule.c`
  - `src/core/heartbeat_dispatch.c`
  - shared private contract: `src/core/heartbeat_internal.h`
- Core config is split into:
  - `src/core/conf.c`
  - `src/core/conf_defaults.c`
  - `src/core/conf_validation.c`
  - shared private contract: `src/core/conf_internal.h`

## Current high-LOC hotspots (status after this update)

- `src/modules/networking/networking_module.c` (876 LOC)
- `src/modules/packages/packages_module.c` (837 LOC)
- `src/modules/metrics/metrics_module.c` (762 LOC)
- `src/modules/man/man_render.c` (render/API orchestration after split)
- `src/render/template_render.c` (516 LOC)

These remain the primary extraction targets in next iterations.

## Implemented in this update (modules extraction start)

`src/modules/metrics/*` now has the first real extraction step beyond thin adapters:

- Added `include/miniweb/modules/metrics_internal.h` for private module-internal contracts (`MetricSample` + JSON assembly helpers).
- Expanded `src/modules/metrics/metrics_json.c` from compatibility shim into concrete JSON section builder functions:
  - history section serialization
  - cpu/memory/load/os/uptime/disks/top-ports section assembly
- Slimmed `src/modules/metrics/metrics_module.c` by delegating those JSON responsibilities to `metrics_json.c`.

This establishes a repeatable module pattern: `*_module.c` owns orchestration and route/handler lifecycles; `*_json.c` owns serialization details; `*_service.c` remains the service boundary.


## Implemented in this update (metrics process extraction)

`src/modules/metrics/*` extraction continued with a concrete split of process-focused responsibilities:

- Added `src/modules/metrics/metrics_process.c` with:
  - process table snapshot acquisition (`sysctl KERN_PROC_ALL`)
  - user resolution helper (`getpwuid_r`)
  - process JSON section builder (`top_cpu_processes`, `top_memory_processes`, `process_stats`)
  - exported process collectors (`metrics_get_top_cpu_processes`, `metrics_get_top_memory_processes`, `metrics_get_process_stats`)
- `src/modules/metrics/metrics_module.c` now delegates process serialization to `metrics_process_append_json_sections()` and keeps endpoint/snapshot orchestration behavior.
- `include/miniweb/modules/metrics_internal.h` now documents private JSON/process helper contracts with Doxygen comments.

Current LOC focus after this step is still man/networking/packages plus remaining metrics orchestrator cleanup.

## Modules extraction plan (`src/modules/*`)

### Phase 1 — Metrics (in progress, advanced)

1. ✅ Extracted JSON section builders into `metrics_json.c`.
2. ✅ Extracted process snapshot/sorting/aggregation helpers into `metrics_process.c`.
3. Next:
   - Extract ring/snapshot cache lifecycle into `metrics_snapshot.c`.
   - Keep `metrics_module.c` as routing + endpoint orchestration only.

### Phase 2 — Man module

✅ Completed in this update.

Delivered layout:

1. `man_query.c` (query parsing/validation).
2. `man_index.c` (index/search data prep, path resolution, catalog/page list/search helpers).
3. `man_render.c` (rendering pipeline, response payload shaping, cache lifecycle, API/render handlers).
4. `man_module.c` preserved as service-facing facade for route wiring/cleanup delegation.
5. `man_service.c` preserved as external service facade and `man_json.c` remains serializer-only.

Phase completion criteria are now satisfied; behavior is kept compatible at endpoint contract level.

### Phase 3 — Networking module

One-round extraction target (same implementation batch as Man):

1. Split `networking_module.c` into:
   - `networking_scan.c` (iface/route/socket collection)
   - `networking_transform.c` (normalization and filtering)
   - `networking_render.c` / `networking_json.c` (serialization)
2. Ensure OpenBSD-specific sysctl/socket code stays encapsulated in scan layer.
3. Phase completion criteria:
   - `networking_module.c` keeps handler registration and top-level flow control only.
   - scan layer owns all OS-collection details and raw record acquisition.
   - transform layer performs deterministic filtering/sorting independent of HTTP concerns.
   - render/json layer is serializer-only and receives pre-normalized data.

### Phase 4 — Packages module

One-round extraction target (same implementation batch as Man + Networking):

1. Split `packages_module.c` into:
   - `packages_backend.c` (pkg_info/pkg_* subprocess and parsing)
   - `packages_cache.c` (optional short-lived cache)
   - `packages_render.c` / `packages_json.c` (serialization)
2. Keep HTTP handler thin and deterministic.
3. Phase completion criteria:
   - backend layer encapsulates subprocess invocation, parsing, and error mapping.
   - cache layer (if enabled) is bounded/TTL-based and explicitly invalidated.
   - module entrypoint remains side-effect-light and testable with backend/cache seams.

## Next execution batch (apply together)

The next extraction round should land **Phases 2-4 together** in one cohesive pass:

1. ✅ Man split (`man_query.c`, `man_index.c`, `man_render.c`) completed with facade preserved.
2. Networking split (`networking_scan.c`, `networking_transform.c`, `networking_render.c`/`networking_json.c`) with OpenBSD specifics isolated to scan.
3. Packages split (`packages_backend.c`, `packages_cache.c`, `packages_render.c`/`packages_json.c`) with deterministic HTTP handler.


## OpenBSD-style direction (unchanged)

1. Keep single-purpose files under ~150-250 LOC where feasible.
2. Prefer narrow `static` functions and one exported concern per compilation unit.
3. Keep data ownership explicit (`*_acquire`, `*_release`, `*_cleanup`).
4. Reduce cross-module state: move globals behind clear accessor APIs.
5. Stabilize branch-predictable fast paths in network + router (early return style).

## Benchmark hardening backlog

- Add `benchmark.sh` preflight mode (`--check`) to validate toolchain and endpoint reachability without running `wrk`.
- Add timeout-safe endpoint retries with capped exponential backoff before marking HEALTH_FAILED.
- Emit machine-readable summary (`benchmark_assets/summary.json`) for CI dashboards.
- Add lightweight smoke target: run 3 fast endpoints for 5s each to catch regressions pre-merge.
