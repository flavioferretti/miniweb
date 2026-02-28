# MiniWeb refactor track (post HTTP response split)

## Scope analyzed in `src/`

- `src/http/`: high complexity surface, request/response marshalling, header parsing, static file path.
- `src/net/`: accept loop, worker handoff, queueing, kqueue lifecycle.
- `src/router/`: route lookup tables, URL registry, module attachment.
- `src/render/`: template expansion and page composition logic.
- `src/core/`: config loading, logging, heartbeat management.
- `src/modules/*`: feature modules (metrics/man/networking/packages) and JSON rendering.
- `src/storage/`: sqlite connection/schema/statement wrappers.
- `src/platform/openbsd/`: OpenBSD specific security hooks.

## What was extracted now

`src/http/response.c` was split into narrow files to reduce per-file LOC and isolate responsibilities:

- `src/http/response_api.c`: response lifecycle + serialization.
- `src/http/response_helpers.c`: request header/proxy helpers and generic response helpers.
- `src/http/response_file.c`: file serving path, cache use, stream fallback.
- `src/http/response_io.c`: robust write/writev retry loops.
- `src/http/response_pool.c`: response object pool shards.
- `src/http/response_file_cache.c`: sharded static file cache + lifecycle.
- `include/miniweb/http/response_internal.h`: private shared contracts.

## OpenBSD-style direction (next)

1. Keep single-purpose files under ~150-250 LOC where feasible.
2. Prefer narrow `static` functions and one exported concern per compilation unit.
3. Keep data ownership explicit (`*_acquire`, `*_release`, `*_cleanup`).
4. Reduce cross-module state: move globals behind clear accessor APIs.
5. Stabilize branch-predictable fast paths in network + router (early return style).

## Recommended next extraction sequence

1. `src/render/template_render.c` (515 LOC)
   - split parsing, placeholder expansion, filesystem reads, layout composition.
2. `src/core/heartbeat.c` (366 LOC)
   - split state machine/timer arithmetic/output renderer.
3. `src/router/url_registry.c` (324 LOC)
   - split static table init, lookup, and reverse routing helpers.
4. `src/core/conf.c` (301 LOC)
   - split parser, defaults, and validation phases.
5. `src/http/utils.c` (245 LOC)
   - split MIME detection, URL decode, and sanitizers.

## Benchmark hardening backlog

- Add `benchmark.sh` preflight mode (`--check`) to validate toolchain and endpoint reachability without running `wrk`.
- Add timeout-safe endpoint retries with capped exponential backoff before marking HEALTH_FAILED.
- Emit machine-readable summary (`benchmark_assets/summary.json`) for CI dashboards.
- Add lightweight smoke target: run 3 fast endpoints for 5s each to catch regressions pre-merge.
