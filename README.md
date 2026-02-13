# MiniWeb (libmicrohttpd) — OpenBSD-oriented lightweight web server

MiniWeb is a compact HTTP server written in C and built on top of `libmicrohttpd`, designed primarily for OpenBSD environments.  
It combines a small route dispatcher, a simple template engine, system metrics endpoints, manual-page browsing APIs, and static file serving.

> This README has been **renewed and extended using `PROJECT_ANALYSIS.md`** to reflect the current project state, architecture, constraints, and recently clarified issues.

---

## Current project status (actual)

### What is implemented
- HTTP daemon entrypoint with CLI config, daemon startup, and signal handling (`src/main.c`).
- Centralized route table + dynamic path checks (`src/routes.c`).
- Helpers for HTTP responses and shell execution (`src/http_utils.c`).
- Metrics collection and JSON assembly (`src/metrics.c`).
- Man-page API/search/rendering (`src/man.c`).
- Placeholder-based HTML template rendering (`src/template_engine.c`).

### Maturity snapshot
- **Strengths**: clean modular split by responsibility, practical API set, explicit OpenBSD hardening intent.
- **Limitations**: several features still rely on shell pipelines (`popen`), limited automated testing, and strong platform coupling to OpenBSD APIs/tooling.

---

## Build and platform notes

This project is currently **OpenBSD-centric**:
- Makefile enables `-D__OpenBSD__` and depends on OpenBSD headers/APIs (e.g., `uvm`, `sysctl`, `pledge`, `unveil`).
- Linker flags and hardening profile assume OpenBSD userland layout.

### Build
```bash
make clean && make
```

---

## Runtime options (CLI)

```bash
build/miniweb -h
Usage: build/miniweb [options]
Options:
  -p PORT      Port to listen on (default: 9001)
  -b ADDR      Address to bind to (default: 127.0.0.1)
  -t NUM       Thread pool size (default: 4)
  -c NUM       Max connections (default: 1000)
  -v           Enable verbose output
  -h           Show this help
```

---

## HTTP surface

### Dashboard + docs
- `GET /` — main dashboard rendered through templates.

### Metrics API
- `GET /api/metrics` — runtime system metrics (CPU, memory, swap, load, disk, network, ports).
- Internal cache/locking logic exists to reduce contention and repeated expensive reads.

### Man API
- `GET /api/man/sections`
- `GET /api/man/pages?section=X`
- `GET /api/man/search?q=query`
- `GET /man/{area}/{section}/{page}` — rendered manual content (format route behavior depends on implementation).

### Static
- `GET /static/*` — static assets.
- `GET /favicon.ico`

---

## Security posture

### Present hardening
- OpenBSD sandbox model via `unveil()` and `pledge()`.
- Static-serving traversal checks (`..`, `//`) and `/static/` confinement.
- Input sanitization in selected command paths.

### Known risk areas
- Shell-command dependence through `popen` in metrics and man workflows increases attack surface.
- `safe_popen_read()` currently enforces size limits, but timeout behavior is not actually implemented despite historical comments.
- Full-buffer file/template reads can be improved with streaming approaches.
- Metrics JSON construction is a long sequence of `snprintf` fragments and static buffers (fragile as payload grows).

---

## Resolved/updated items in this documentation refresh

This update resolves stale README claims and adds missing project-state transparency:

1. **Resolved outdated project description** by aligning docs with the real module layout and route/API behavior.
2. **Resolved ambiguity about platform support** by explicitly documenting OpenBSD-first constraints.
3. **Added explicit risk register** (shell pipelines, timeout gap, JSON fragility, buffering approach).
4. **Added actionable engineering roadmap** based on current technical debt.

> Note: this specific change is a **documentation improvement**; it does not change runtime behavior.

---

## Recommended next engineering steps

1. Replace shell-heavy metrics/man command paths with safer syscall/exec-argv designs.
2. Implement real subprocess timeout handling for `safe_popen_read()`.
3. Add portability boundaries (feature gates + non-OpenBSD stubs).
4. Add validation tests (route matching, template rendering, integration endpoints).
5. Fix Makefile dependency inaccuracies (`http_utils.o`, `man.o` prerequisites).

---

## License

BSD 3-Clause
