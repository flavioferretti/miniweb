# Project Analysis: miniweb_libmicrohttpd

## Overview
MiniWeb is a lightweight OpenBSD-oriented HTTP server built in C on top of `libmicrohttpd`.
It combines:
- a route dispatcher,
- a minimal template engine,
- a metrics API,
- a man-page API and renderer,
- static file serving,
- OpenBSD hardening (`unveil`, `pledge`).

## Architecture
- **Entry point and runtime config** are in `src/main.c`, including CLI parsing, daemon startup, and signal handling.
- **Routing** is centralized in `src/routes.c` with a fixed route table plus dynamic path checks.
- **Response and shell utility helpers** live in `src/http_utils.c`.
- **System metrics collection** and JSON generation are in `src/metrics.c`.
- **Manual pages API/rendering** are implemented in `src/man.c`.
- **HTML composition** is done with a custom placeholder-based template engine in `src/template_engine.c`.

## Build & Platform assumptions
- The Makefile is strongly OpenBSD-specific (`-D__OpenBSD__`, `pledge`, `unveil`, OpenBSD sysctl/uvm headers).
- Link flags include local OpenBSD paths and hardening options.
- This likely does not build out-of-the-box on Linux/macOS without portability shims.

## Security posture
### Positives
- OpenBSD sandboxing posture is explicit via `unveil()` and `pledge()`.
- Static serving has explicit traversal checks (`..`, `//`) and is constrained to `/static/` URL prefix.
- Some command inputs are sanitized before shell execution.

### Risks / Gaps
- Multiple features rely on shell pipelines via `popen` (metrics and man search/render flows); this increases parsing and injection risk surface.
- `safe_popen_read()` has size limiting but no explicit timeout despite the comment claiming timeout behavior.
- Template and static file loading are full-buffer reads and not stream-based.
- JSON response building in metrics is done with many `snprintf` fragments and static buffers; this is fragile for future growth.

## Code quality observations
- Code is modular and fairly readable with clear separation by domain.
- There are signs of active debugging instrumentation in `metrics.c` (verbose logging and comments labeled as debug/instrumented).
- Some formatting consistency issues and minor make dependency inaccuracies are present.

## Functional coverage
- Dashboard and docs pages from templates.
- API endpoints for metrics and man database browsing/search.
- Dynamic man rendering endpoint supporting html/pdf/markdown-like output.
- Static assets and favicon endpoint.

## Recommended next steps
1. **Stabilize metrics subsystem**
   - Replace shell-based process/port scraping with direct syscalls or safer APIs where possible.
   - Implement actual timeout handling in `safe_popen_read` (e.g., fork+exec with alarm/poll).
2. **Harden command execution paths**
   - Introduce strict allowlists/escaping strategy per command argument.
   - Prefer `execve`-style argument vectors over shell command strings.
3. **Improve portability boundaries**
   - Add feature-gated OpenBSD code and fallback stubs for non-OpenBSD builds.
4. **Add validation tests**
   - Route matching tests, template render tests, and integration tests for core endpoints.
5. **Fix build rule dependencies**
   - Update object prerequisites in `Makefile` (`http_utils.o` and `man.o` currently point to `main.c`).

## Quick maturity assessment
- **Strengths:** clear modular decomposition, OpenBSD hardening intent, practical API surface.
- **Weaknesses:** heavy shell command dependence, limited automated verification, platform coupling.
- **Overall:** promising OpenBSD-focused utility server, best suited for controlled environments after tightening command execution and adding tests.
