# Release Notes (da5a450 → HEAD)

## New features
- Added a CLI log override switch (`-l FILE`) so log output destination can be changed at process startup without editing config files.
- Added lightweight in-memory metrics history (ring buffer) and exposed history in `/api/metrics` for dashboard timeline visualizations.
- Added dashboard overview charts (CPU and memory history) driven by the metrics history payload.
- Added/expanded man-page API and renderer behaviors including static cache handling for rendered man outputs.

## Improvements
- Improved high-concurrency behavior by hardening response allocation with a larger pool and heap fallback when pool entries are exhausted.
- Improved listener resilience under file descriptor pressure (`EMFILE`/`ENFILE`) by using a spare-fd shedding pattern in the accept loop.
- Optimized metrics collection by reducing repeated process queries and using shared snapshots for multiple metrics sections.
- Reduced metrics overhead by dropping expensive network-interface enumeration from `/api/metrics`.
- Improved chart rendering quality by smoothing flvChart lines and disabling default point circles for cleaner visuals.
- Improved dashboard update behavior to preserve canvases across refreshes and prevent chart flicker/disappearance.
- Updated route/docs alignment and architecture documentation to better match actual runtime behavior and caching model.

## Bug fixes
- Fixed potential crash path when response object allocation fails under burst load.
- Fixed dashboard tab/payload mismatches after metrics schema changes (removed obsolete Network tab usage and surfaced Top Ports data).
- Fixed event-loop stall risk during fd exhaustion conditions by draining one pending connection and restoring reserve capacity.

## Documentation
- Added and iteratively refined Doxygen documentation coverage across C source files.
- Updated man page and README for new CLI options and revised performance/reporting sections.
- Refreshed diagrams and documentation sections to track routing/caching/security/runtime changes.

## UI/Frontend adjustments
- Multiple dashboard/template refinements (layout, controls, styling consistency, process table semantics, performance page updates).
- Included flvChart integration/assets and subsequent polish updates.

## Notes
- This release window includes multiple merge commits that bundled feature branches (performance, observability, stability, and docs updates).
- For exact commit-by-commit detail, run: `git log --oneline da5a450..HEAD`.
