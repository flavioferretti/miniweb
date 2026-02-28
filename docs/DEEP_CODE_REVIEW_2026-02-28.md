# Deep Analytical Code Review (2026-02-28)

Scope: static review focused on:
1. crash-under-load causes,
2. memory leaks/resource leaks,
3. programming errors and bad practices.

## Executive summary

## Resolution status (implemented)

All high-priority findings from this report have now been addressed in code:
- fixed duplicate handler invocation in worker dispatch,
- synchronized idle sweep with connection-pool locking,
- hardened response-header length accounting and assembly bounds checks,
- added startup-failure descriptor cleanup in server run path,
- removed duplicate `Content-Disposition` insertion,
- added `pthread_create` error handling,
- enforced `trusted_proxy` checks before honoring forwarded headers,
- switched process signal setup to `sigaction`,
- added explicit shutdown cleanup for HTTP global cache data and man module cache/semaphore resources.

The most likely crash-under-load causes are in the networking + response hot path:
- **double handler execution per request** in worker loop,
- **data race during idle sweep** against connection-pool mutations,
- **unsafe header-length bookkeeping** that can drive out-of-bounds pointer math.

Resource-lifetime hygiene is generally improved versus earlier revisions, but there are still several **resource leak** paths (mainly file descriptors / kqueue descriptors on startup failures and long-lived global caches without explicit teardown).

---

## 1) Crash-under-load causes (high priority)

### A. Worker calls each route handler twice (functional corruption + crash amplifier)
**Evidence:** `miniweb_worker_thread()` invokes the matched handler once unconditionally, then invokes it again to collect `handler_result`.  
Location: `src/net/worker.c` lines 136 and 141.

**Why this is dangerous under load:**
- duplicate writes on same socket,
- duplicate side effects in handlers,
- higher probability of write-after-close / protocol corruption,
- magnifies CPU and contention by ~2x in request path.

### B. Data race in `sweep_idle()` against connection pool ownership
**Evidence:** idle sweep reads `rt->pool.connections[fd]` and dereferences `c->last_activity` without holding pool lock.  
Location: `src/net/server.c` lines 68-73.

**Why this is dangerous under load:**
- worker threads concurrently free/reassign entries under `miniweb_connection_pool_t.lock`,
- unsynchronized read here can observe stale/freed connection metadata,
- can produce use-after-free style crashes or random closes.

### C. Header bookkeeping can go out-of-bounds (`snprintf` return misuse)
**Evidence:** `http_response_add_header()` increments `headers_len` by `snprintf()` return value even when output was truncated.  
Location: `src/http/response.c` lines 459-464.

**Why this is dangerous:**
`snprintf` returns the bytes *that would have been written*. If that exceeds remaining capacity, `headers_len` becomes logically larger than the actual buffer, and subsequent append operations may compute invalid write pointers.

### D. HTTP header assembly lacks robust overflow/truncation guards
**Evidence:** `http_response_send()` stores `snprintf` return in `header_len`, then continues appending using `header + header_len` without validating `header_len` range.  
Location: `src/http/response.c` lines 607-629.

**Why this is dangerous under load:**
If larger-than-expected headers are produced (custom headers/cookies/etc.), pointer arithmetic can become invalid and lead to undefined behavior.

---

## 2) Memory leaks / resource leaks

### E. Startup failure paths leak descriptors/resources in `miniweb_server_run`
**Evidence:** multiple early `return -1` paths after socket creation/bind/kqueue setup do not close previously acquired fds.  
Location: `src/net/server.c` lines 90-110.

**Examples:**
- `inet_pton` failure after socket opened,
- `bind/listen` failure after socket opened,
- `kqueue()` or `kevent()` failure after listen fd already open.

### F. No explicit teardown of global response/file-cache allocations
**Evidence:** global pools/caches initialized in `http_handler_globals_init()` via `pthread_once`, but no symmetric shutdown cleanup is called.  
Location: `src/http/response.c` lines 113-127 and cache allocations in lines 362-366 and 316-325.

**Impact:**
- long-running process: bounded but persistent memory retention,
- tooling (ASan/LSan) flags these as still reachable/leaked at process exit.

### G. Man module global caches/semaphore have init but no teardown
**Evidence:** semaphore initialized via `sem_init` and in-memory cache slots allocate heap bodies; no module shutdown path frees all cache bodies/destroys semaphore.  
Location: `src/modules/man/man_module.c` lines 93-104 and 203-273.

**Impact:**
Primarily process-lifetime leaks; operationally acceptable for daemon lifetime, but still poor resource lifecycle discipline.

---

## 3) Programming errors / bad practices

### H. Duplicate `Content-Disposition` header insertion
**Evidence:** identical header add call appears twice consecutively.  
Location: `src/http/response.c` lines 919-920.

**Impact:** noisy protocol output; can surprise clients/proxies.

### I. Unchecked `pthread_create` return values
**Evidence:** worker thread creation loop ignores `pthread_create` failures.  
Location: `src/net/server.c` lines 111-112.

**Impact:** server may run with fewer workers than configured without visibility; can look like random load collapse.

### J. Unsafely trusting forwarding headers without trusted-proxy enforcement
**Evidence:** configuration includes `trusted_proxy`, but `http_request_get_client_ip()` accepts `X-Real-IP` / `X-Forwarded-For` unconditionally.  
Locations: config definition/use in `src/core/conf.c` lines 53, 118-119; header trust in `src/http/response.c` lines 760-779.

**Impact:** client IP spoofing in logs/rate-limits/audit decisions.

### K. Startup signal handling uses `signal()` instead of `sigaction()`
**Evidence:** SIGINT/SIGTERM set with `signal()`.  
Location: `src/app_main.c` lines 112-113.

**Impact:** less portable/robust semantics (handler reset behavior varies historically).

---

## Recommended remediation order

1. **Fix A + B immediately** (double handler invocation, idle-sweep race).
2. **Fix C + D next** (header length accounting/overflow guards).
3. Add **single-exit cleanup labels** in `miniweb_server_run` for fd/resource unwind.
4. Add explicit shutdown hooks for module/global caches (response cache + man cache/semaphore).
5. Enforce trusted-proxy policy before consuming forwarded headers.
6. Harden thread creation and signal handling (`pthread_create` checks, `sigaction`).

---

## Load-test hypotheses to validate after fixes

- p95/p99 latency should materially improve once double-handler invocation is removed.
- crash frequency under high concurrency should drop after sweep lock correctness.
- memory/FD plateau behavior should stabilize after startup-failure cleanup and explicit cache teardown hooks.
