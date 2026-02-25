
MINIWEB(1) - General Commands Manual

# NAME

**miniweb** - OpenBSD-native C99 HTTP server with modular architecture

# SYNOPSIS

**miniweb**
\[**-hv**]
\[**-b**&nbsp;*address*]
\[**-c**&nbsp;*connections*]
\[**-f**&nbsp;*file*]
\[**-l**&nbsp;*logfile*]
\[**-p**&nbsp;*port*]
\[**-t**&nbsp;*threads*]

# DESCRIPTION


---
![screenshot](https://raw.githubusercontent.com/flavioferretti/miniweb/refs/heads/main/docs/screenshot.png)
---
![call graph](https://raw.githubusercontent.com/flavioferretti/miniweb/refs/heads/main/docs/miniweb_diagram.svg)
---

**miniweb**
is a lightweight HTTP server written in C99 for OpenBSD.
It exposes a system dashboard and JSON APIs for metrics, networking
diagnostics, manual page browsing, and package management.

The runtime uses a single
kqueue(2)
dispatcher thread and a configurable worker-thread pool.
All modules are registered through a central attach API and maintain
their own ring buffer caches, updated by a global heartbeat scheduler.

**miniweb**
does not implement TLS directly.
Run it behind
relayd(8)
for TLS termination.

# OPTIONS

**-b** *address*

> Bind to an IPv4 address.
> Default is
> **127.0.0.1**.

**-c** *connections*

> Maximum concurrent connections.
> Clamped to the compile-time limit of
> **4096**.
> Default is
> **1280**.

**-f** *file*

> Load configuration from
> *file*.
> A parse error in an explicitly supplied file is fatal.

**-h**

> Print usage and exit.

**-l** *logfile*

> Append log output to
> *logfile*
> instead of stderr.

**-p** *port*

> TCP listen port.
> Default is
> **9001**.

**-t** *threads*

> Worker thread count.
> Clamped to the range 1&#8211;32.
> Default is
> **4**.

**-v**

> Enable verbose logging.
> Equivalent to setting
> **verbose yes**
> in the configuration file.

# CONFIGURATION FILE

On startup
**miniweb**
searches for a configuration file in the following order (first file found
wins):

1.	Explicit path from the
	**-f**
	flag.
2.	*./miniweb.conf*
3.	*$HOME/.miniweb.conf*
4.	*/etc/miniweb.conf*

If no file is found, compiled-in defaults are used and startup continues
normally.
CLI flags always override file values.

The file format is one directive per line:

	key value

Keys are case-insensitive.
Lines beginning with
'`#`'
and blank lines are ignored.

Supported keys:

**port**

> TCP listen port.
> Default:
> **9001**.

**bind**, **bind\_addr**

> IPv4 address to bind to.
> Default:
> **127.0.0.1**.
> Use
> **0.0.0.0**
> to listen on all interfaces.

**threads**

> Worker thread count.
> Clamped to 1&#8211;32.
> Default:
> **4**.

**max\_conns**

> Maximum concurrent connections.
> Default:
> **1280**.

**conn\_timeout**

> Idle connection timeout in seconds.
> Default:
> **30**.

**max\_req\_size**

> Maximum HTTP request size in bytes.
> Must not exceed the compile-time limit of
> **16384**.
> Default:
> **16384**.

**mandoc\_timeout**

> Timeout for
> mandoc(1)
> subprocess invocations, in seconds.
> Default:
> **10**.

**static\_dir**

> Path to the static assets directory.
> Default:
> *static*.

**templates\_dir**

> Path to the HTML templates directory.
> Default:
> *templates*.

**mandoc\_path**

> Path to the
> mandoc(1)
> binary.
> Default:
> */usr/bin/mandoc*.

**trusted\_proxy**

> IPv4 address of a reverse proxy whose
> `X-Forwarded-For`,
> `X-Forwarded-Proto`,
> and
> `X-Real-IP`
> headers are honoured.
> Default:
> **127.0.0.1**.

**verbose**

> Enable verbose logging.
> Accepted values:
> **yes**, **no**, **true**, **false**, **1**, **0**.
> Default:
> **no**.

**log\_file**

> Log file path.
> Empty or absent means stderr.

# RUNTIME ARCHITECTURE

**miniweb**
uses a two-tier threading model.

The
*dispatcher thread*
(main) owns the
kqueue(2)
file descriptor, runs the accept loop, and enqueues ready connections into a
ring-buffer work queue.
It never calls a route handler.

The
*worker pool*
(N threads, configurable via
**-t**)
dequeues connections, reads the full HTTP request, parses the request line
and headers, dispatches the matching route handler, and either closes the
socket or re-arms it for keep-alive.

Compile-time hard limits:

`MAX_EVENTS`

> 256 &#8212;
> kevent(2)
> batch size per iteration.

`MAX_CONNECTIONS`

> 4096 &#8212; connection pool size.

`THREAD_POOL_SIZE`

> 32 &#8212; maximum worker threads.

`REQUEST_BUFFER_SIZE`

> 16384 &#8212; per-connection receive buffer.

`LISTEN_BACKLOG`

> 1024 &#8212;
> listen(2)
> backlog.

`QUEUE_CAPACITY`

> 4096 &#8212; work queue ring-buffer capacity.

`MAX_KEEPALIVE_REQUESTS`

> 64 &#8212; requests per keep-alive connection.

# KQUEUE DISPATCHER

The listening socket is registered with
`EVFILT_READ` | `EV_CLEAR`
so that the backlog counter resets after every
kevent(2)
return.

Client sockets are registered with
`EV_ADD` | `EV_DISPATCH`.
`EV_DISPATCH`
automatically disables the event after the first delivery, eliminating the
race where multiple workers could receive the same file descriptor.

Each main-loop iteration uses a 1-second timeout so that idle connection
sweeping runs at least once per second regardless of traffic.
The sweep closes connections whose
`last_activity`
timestamp is older than
**conn\_timeout**.

When the OS file descriptor table is full
(`EMFILE / ENFILE`),
the dispatcher closes a reserved
*/dev/null*
spare descriptor, accepts and immediately drops one pending client to prevent
the listen socket from busy-looping, then reopens the spare.

The
`udata`
pointer in each
kevent(2)
structure is validated against a per-file-descriptor generation counter before
dispatching.
Stale pointers from recycled descriptors are silently ignored.

# WORKER POOL AND WORK QUEUE

Workers are identical POSIX threads sharing a single ring-buffer work queue
protected by a mutex and a condition variable.

The dispatcher enqueues connections non-blocking; if the queue is full the
connection is dropped with a
**503**
response.
Workers block on
**pthread\_cond\_wait**()
until a connection is available or the server shuts down.

Within a worker, the read loop accumulates data from the non-blocking socket
until the HTTP header terminator
(`\r\n\r\n`)
is found.
If
`EAGAIN`
is returned before headers are complete, the
kqueue(2)
event is re-enabled with
`EV_ENABLE`
and the worker returns without closing the connection.

Keep-alive connections are re-armed after each request.
A connection is closed unconditionally after
`MAX_KEEPALIVE_REQUESTS`
(64) requests on the same socket.

# CONNECTION POOL

The connection pool is a flat array of
*connection\_t*
structs indexed by file descriptor.
Allocation and deallocation are O(1) via a LIFO free-stack.
Each slot carries the client socket, peer address, receive buffer,
activity timestamps, request count, and a generation counter.
The generation counter allows the dispatcher to detect stale
kevent(2)
`udata`
pointers: if the counter in the event does not match the current counter for
that file descriptor, the event is a stale reference to a recycled socket and
is ignored.

# HTTP LAYER

## Response pool

**http\_response\_create**()
allocates a response object from a per-thread shard of a static response pool
(16 shards, 1024 slots each) to avoid a
malloc(3)
call on every request.
If the pool is exhausted it falls back to
calloc(3).

## Serialisation

**http\_response\_send**()
serialises the status line, standard headers
(Content-Type, Content-Length, Connection, Server),
optional custom headers, and the body using a single
writev(2)
call when a body is present, or
write(2)
for header-only responses.
Write retries handle
`EAGAIN / EWOULDBLOCK`
on non-blocking sockets.

## Subprocess execution

**safe\_popen\_read\_argv**()
forks, redirects stdout to a pipe, redirects stderr to
*/dev/null*,
and reads up to
*max\_size*
bytes with a wall-clock timeout enforced by
poll(2).
On timeout the child is sent
`SIGKILL`
before
waitpid(2).

# ROUTING AND MODULES

Routing is fully modular through the module attach API.
**init\_routes**()
in
*src/router/url\_registry.c*
builds a
*miniweb\_module*
list and lets each enabled module attach its own endpoints through
**router\_register**().

Routes are stored in two tables:

*	Exact routes: flat array of
	*struct route*
	(up to
	`MAX_ROUTES`
	&equals; 32).
*	Prefix routes: array of
	*struct prefix\_route*
	(up to 16) for dynamic paths like
	*/man/\*&zwnj;*,
	*/static/\*&zwnj;*,
	and
	*/api/packages/\*&zwnj;*.

**route\_match**()
resolves a handler in two passes:

1.	Exact match: linear scan comparing method and path strings.

2.	Prefix match: checks prefix routes with optional minimum slash requirements.

When a path exists but the method doesn't match, a
**405 Method Not Allowed**
response is sent with an
`Allow`
header built by
**route\_allow\_methods**().
Unknown paths receive
**404 Not Found**.

# MODULE SYSTEM

Modules are registered through
**miniweb\_module\_attach\_enabled**()
in
*src/router/module\_attach.c*.
Each module provides:

*	*name*:
	Module identifier.
*	**init**():
	Optional initialization callback.
*	**attach\_routes**():
	Registers endpoints with the router.
*	**shutdown**():
	Optional cleanup callback.
*	*enabled\_by\_default*:
	Flag for automatic enabling.

All built-in modules (views, metrics, networking, man, packages) are attached
at startup through this contract.

# CACHING ARCHITECTURE

**miniweb**
employs multiple caching layers for optimal performance.

## Ring buffer caches (per-module)

	*Module* *Cache Type* *Size* *TTL* *Description*  
	Metrics  Ring buffer  1MB    1s    Samples system metrics every second.  
	Networking Ring buffer  1MB    1s    Samples network stats every second.  
	Packages Ring buffer  2MB    30s   Caches all package queries (search, info, files, list, which).  
	Man pages Sharded render cache 16x128 slots 600s  Caches rendered man page content.

## Static file cache

*src/http/response.c*
implements a sharded in-memory file cache:

*	16 independent shards, each with 32 slots.
*	Files up to 256 KiB are cached.
*	Two-stage admission policy (requires 2 requests before caching).
*	Token bucket rate limiting (8 inserts/sec per shard).
*	120-second TTL with LRU eviction.

## Template cache

*src/render/template\_render.c*
preloads all HTML templates at startup and refreshes them every 60 seconds.

## Hot view cache

*src/router/route\_table.c*
caches rendered HTML for top-level pages
(*/*, */docs*, */networking*, */packages*, */apiroot*)
with a 10-second TTL.

All ring buffer caches track hit/miss ratios and log them periodically.

# HEARTBEAT SCHEDULER

*src/core/heartbeat.c*
provides a generic periodic task scheduler.
Up to
`HB_MAX_TASKS`
(32) named tasks can be registered with
**heartbeat\_register**().
Each task specifies a callback, an opaque context pointer, a period in
seconds, and an initial delay.

**heartbeat\_register**()
returns:

*	`HB_REGISTER_INSERTED`
	(1) on success.
*	`HB_REGISTER_DUPLICATE`
	(0) for a duplicate name (non-fatal).
*	`HB_REGISTER_ERROR`
	(-1) on invalid input or a full table.

**heartbeat\_start**()
spawns a single background thread that fires due tasks and tracks overrun
counts per task.
**heartbeat\_shutdown**() *drain*
stops the scheduler; when
*drain*
is non-zero, every active task is executed one final time before the thread
exits.

Registered heartbeat tasks:

	*Task Name*  *Period*  *Module*  *Purpose*  
	"metrics.sample" 1s        Metrics   Sample system metrics.  
	"networking.sample" 1s        Networking Sample network stats.  
	"packages.snapshot" 60s       Packages  Refresh package list cache.  
	"man.cache_cleanup" 60s       Man pages Clean expired cache entries.

# MODULES

## Views module

(*src/router/route\_table.c*)
Provides the template-rendered HTML dashboard pages:
*/*, */docs*, */networking*, */packages*, */apiroot*, */favicon.ico*, */static/\*&zwnj;*
Uses the hot view cache (10s TTL) for top-level pages.

## Metrics module

(*src/modules/metrics/*)
Provides
*/api/metrics*
with comprehensive system metrics.

Collection methods:
sysctl(3)
(`KERN_CPTIME`, `VM_UVMEXP`, `KERN_BOOTTIME`, `KERN_PROC_ALL`),
swapctl(2),
getloadavg(3),
uname(3),
getmntinfo(3).

Caching: 1MB ring buffer with 1-second samples, 120-sample history window,
heartbeat-driven updates.

## Networking module

(*src/modules/networking/*)
Provides
*/api/networking*
with network diagnostics.

Collection methods:
sysctl(3)
(`NET_RT_DUMP`),
parsing of
*/etc/resolv.conf*,
getifaddrs(3)
with
*if\_data*.

Caching: 1MB ring buffer with 1-second samples, pre-built JSON snapshot updated
every second.

## Man pages module

(*src/modules/man/*)
Provides rendered man pages and JSON API.

Endpoints:
*/man/{area}/{section}/{page}\[.{html|txt|md|pdf|ps}]*,
*/api/man/sections*,
*/api/man/pages?section={s}&area={a}*,
*/api/man/search?q={query}*,
*/api/man/resolve?name={n}&section={s}*.

Rendering: Forks
mandoc(1)
with configurable timeout (default 10s), supports HTML, text, markdown, PDF,
and PostScript output.

Caching: Sharded render cache (16 shards &#215; 128 slots = 2048 cached pages) with
600-second TTL, plus filesystem cache under
*{static\_dir}/man/*.

## Packages module

(*src/modules/packages/*)
Provides package management API via
pkg\_info(1).

Endpoints:
*/api/packages/search?q={query}*,
*/api/packages/info?name={pkg}*,
*/api/packages/which?path={file}*,
*/api/packages/files?name={pkg}*,
*/api/packages/list*.

Caching: 2MB ring buffer with 30-second TTL, stores all query types, hit/miss
tracking, on-demand population (no heartbeat needed).

# STORAGE LAYER

*src/storage/*
contains stub implementations of a planned SQLite3 service layer.
The interfaces are defined but not yet implemented:
**mw\_db\_open**(),
**mw\_db\_close**(),
**mw\_db\_exec\_schema**(),
**mw\_db\_migrate**(),
**mw\_tx\_begin**(),
**mw\_tx\_commit**(),
**mw\_tx\_rollback**(),
**mw\_stmt\_prepare**(),
**mw\_bind\_text**(),
**mw\_bind\_int64**(),
**mw\_bind\_null**(),
**mw\_stmt\_step**(),
**mw\_stmt\_finalize**().

All stub functions currently return
`-1`
(unimplemented).
The headers are stable and intended for Phase 5 of the modularity roadmap.

# OPENBSD SECURITY HARDENING

After worker threads are started,
**apply\_openbsd\_security**()
restricts the process with
unveil(2)
and
pledge(2).

unveil(2)
grants access only to:

*	**templates\_dir**
	(read),
	**static\_dir**
	(read/write/create).
*	*/usr/share/man*, */usr/local/man*, */usr/X11R6/man*
	(read).
*	**mandoc\_path**,
	*/usr/bin/man*, */usr/bin/apropos*, */usr/sbin/pkg\_info*
	(execute).
*	*/bin/sh*
	(execute for fallback popen).
*	*/var/db/pkg*
	(read).
*	*/etc/passwd*, */etc/group*
	(read for process username resolution).
*	*/etc/resolv.conf*
	(read).
*	*/dev/null*
	(read/write).

pledge(2)
promises:

	stdio rpath wpath cpath inet route proc exec vminfo ps getpw

On non-OpenBSD platforms both calls are compiled out.

# LOGGING

*src/core/log.c*
provides a thread-safe, mutex-protected logger.
Log lines are formatted as:

	[YYYY-MM-DD HH:MM:SS] [LEVEL] message

**log\_init**()

> Opens the log file (appending) or defaults to stderr.

**log\_close**()

> Flushes and closes the log file.

**log\_info**(), log\_error

> Always emitted.

**log\_debug**()

> Emitted only in verbose mode.

**log\_errno**()

> Calls
> **log\_error**()
> with
> strerror(3)
> of the current
> *errno*.

# ENDPOINTS REFERENCE

## Views (HTML, template-rendered)

	*Method*  *Path*  *Title*  *Cache*  
	GET       /       Dashboard Hot view (10s)  
	GET       /docs   Documentation Hot view (10s)  
	GET       /networking Networking Hot view (10s)  
	GET       /packages Package Manager Hot view (10s)  
	GET       /apiroot API Index Hot view (10s)

## JSON APIs

	*Method*  *Path*  *Description*  *Cache*  
	GET       /api/metrics System metrics snapshot Ring buffer (1s)  
	GET       /api/networking Networking diagnostics Ring buffer (1s)  
	GET       /api/man/sections Manual sections catalog Static JSON  
	GET       /api/man/{area}/{section} Pages in section On-demand  
	GET       /api/man/search?q={query} Search man pages On-demand  
	GET       /api/man/resolve?name={n}&section={s} Resolve page   On-demand  
	GET       /api/packages/search?q={query} Search packages Ring buffer (30s)  
	GET       /api/packages/info?name={pkg} Package details Ring buffer (30s)  
	GET       /api/packages/which?path={file} File ownership Ring buffer (30s)  
	GET       /api/packages/files?name={pkg} Package files  Ring buffer (30s)  
	GET       /api/packages/list All packages   Ring buffer (30s)

## Rendered manual pages

	*Method*  *Path*  *Description*  *Cache*  
	GET       /man/{area}/{section}/{page} HTML           Sharded (600s)  
	GET       /man/{area}/{section}/{page}.html HTML           Sharded (600s)  
	GET       /man/{area}/{section}/{page}.txt Plain text     Sharded (600s)  
	GET       /man/{area}/{section}/{page}.md Markdown       Sharded (600s)  
	GET       /man/{area}/{section}/{page}.pdf PDF            Sharded (600s)  
	GET       /man/{area}/{section}/{page}.ps PostScript     Sharded (600s)

## Static assets

	*Method*  *Path*  *Description*  
	GET       /static/* CSS, JavaScript, images  
	GET       /favicon.ico Favicon (served from static/assets/favicon.svg)

# PERFORMANCE TUNING GUIDE

## Runtime tunables (CLI/config)

*	**threads**
	: worker threads that execute handlers.
	Increase gradually; too high can increase lock contention.
*	**max\_conns**
	: connection budget.
	Must stay below kernel/file-descriptor limits.
*	**conn\_timeout**
	: idle keep-alive timeout; lower values reclaim sockets faster.
*	**max\_req\_size**
	: hard cap for request header buffering.
*	**mandoc\_timeout**
	: upper bound for man rendering subprocesses.
*	**verbose**
	: keep disabled during benchmarks.

## Compile-time tunables (code constants)

*	Dispatcher/queue sizing:
	`MAX_EVENTS`, `LISTEN_BACKLOG`, `QUEUE_CAPACITY`, `MAX_KEEPALIVE_REQUESTS`.
*	HTTP write path:
	`WRITE_RETRY_LIMIT`, `WRITE_WAIT_MS`.
*	Static asset cache:
	`FILE_CACHE_SHARDS`, `FILE_CACHE_SLOTS`, `FILE_CACHE_MAX_BYTES`,
	`FILE_CACHE_INSERTS_PER_SEC`, `FILE_CACHE_MAX_AGE_SEC`.
*	Module ring buffers:
	`METRICS_RING_BYTES`, `NETWORK_RING_BYTES`, `PKG_RING_BYTES`.

## Caching strategy summary

*	Template cache: preloaded at startup, refreshed every 60s.
*	Hot view cache: 5 slots, 10s TTL.
*	Static file cache: 16 shards &#215; 32 slots, 256 KiB max, 120s TTL.
*	Metrics ring: 1MB, 1s samples, 120-sample history.
*	Networking ring: 1MB, 1s samples, pre-built JSON snapshot.
*	Man page cache: 16&#215;128 slots, 600s TTL, filesystem fallback.
*	Package ring: 2MB, 30s TTL, on-demand population.

# SOURCE LAYOUT

*src/app\_main.c*

> Main entry point: kqueue loop, worker pool, accept, idle sweep, shutdown.

*src/net/server.c*

> kqueue dispatcher and accept loop implementation.

*src/net/connection\_pool.c*

> fd-indexed connection pool with O(1) alloc/free and generation guards.

*src/net/worker.c*

> Worker request read, parse, and route dispatch.

*src/net/work\_queue.c*

> Thread-safe FIFO transport work queue.

*src/platform/openbsd/security.c*

> OpenBSD sandbox boundary setup using
> unveil(2)
> and
> pledge(2).

*src/core/conf.c*

> Configuration file parser and CLI override logic.

*src/core/heartbeat.c*

> Periodic task scheduler (up to 32 named tasks, overrun tracking).

*src/core/log.c*

> Thread-safe logger.

*src/http/response.c*

> Response pool, serialisation, sharded file cache, HTTP send helpers.

*src/http/utils.c*

> JSON escaping, subprocess execution (fork/poll/timeout).

*src/router/module\_attach.c*

> **miniweb\_module\_attach\_enabled**()
> module registration.

*src/router/route\_table.c*

> Static, favicon, and hot-view-cache handlers.

*src/router/router.c*

> **router\_register**()
> and
> **router\_register\_prefix**()
> wrappers.

*src/router/url\_registry.c*

> Route table, prefix routes,
> *view\_routes\[]*,
> **init\_routes**().
> All built-in modules are attached here.

*src/modules/man/man\_module.c*

> Man page rendering, filesystem render cache, sharded RAM cache, and JSON API.

*src/modules/man/man\_service.c*

> Man page service layer (search, metadata).

*src/modules/man/man\_json.c*

> JSON serialisation for man API.

*src/modules/metrics/metrics\_module.c*

> System metrics collection, heartbeat task, ring buffer, and JSON API.

*src/modules/metrics/metrics\_service.c*

> Metrics collection helpers.

*src/modules/metrics/metrics\_json.c*

> JSON serialisation for metrics.

*src/modules/networking/networking\_module.c*

> Networking diagnostics, heartbeat task, ring buffer, and JSON API.

*src/modules/networking/networking\_service.c*

> Network collection helpers.

*src/modules/networking/networking\_json.c*

> JSON serialisation for networking.

*src/modules/packages/packages\_module.c*

> Package management with 2MB ring buffer cache and JSON API.

*src/modules/packages/packages\_service.c*

> Package query helpers.

*src/modules/packages/packages\_json.c*

> JSON serialisation for packages.

*src/render/template\_render.c*

> Template file cache (preloaded at startup, refreshed every 60s) and placeholder
> substitution.

*src/storage/sqlite\_db.c*

> SQLite3 database lifecycle stubs.

*src/storage/sqlite\_schema.c*

> Schema migration and transaction stubs.

*src/storage/sqlite\_stmt.c*

> Prepared statement stubs.

# MODULARITY ROADMAP

Phase 1

> Module attach API and router separation.
> Complete: all modules use
> **miniweb\_module\_attach\_enabled**().

Phase 2

> Single global heartbeat scheduler.
> Complete: metrics and networking both run as named heartbeat tasks.

Phase 3

> Domain decomposition of metrics, networking, man, packages.
> Complete: all modules are split into
> *\*\_module.c*,
> *\*\_service.c*,
> and
> *\*\_json.c*.

Phase 4

> Documentation and architecture transition.
> Complete: README, man page, and all Doxygen blocks updated to reflect current
> state.

Phase 5

> SQLite3 storage integration.
> Stub interfaces are defined; no live backend yet.

Phase 6

> Performance and observability hardening.
> Planned.

# NEXT STEPS FOR MODULARITY

The following tasks are the immediate backlog for achieving low LOC-per-file and
low LOC-per-function targets.

1.	Split
	*src/http/response.c*
	(~973 LOC) into
	*response\_writer.c*,
	*static\_files.c*,
	and
	*mime.c*.
2.	Extract platform-specific collectors into dedicated translation units under
	*src/platform/openbsd/*
	for CPU, memory, disk, process, network interfaces, and routing table.
	Modules must consume only collector APIs; no direct sysctl calls should remain
	inside module files.
3.	Apply function-size limits (soft 40 LOC, review trigger 80 LOC) to
	**miniweb\_server\_run**(),
	**man\_render\_handler**(),
	and
	**build\_system\_metrics\_json**().
4.	Harden the integration test suite: explicit HTTP status checks, JSON key
	presence checks, golden payload fixtures.
5.	Implement the SQLite3 backend (Phase 5): connect
	*src/storage/*
	stubs to real
	**sqlite3\_open\_v2**(),
	prepared statements, and transactions; persist module flags and bounded
	metric snapshots.

# DEVELOPMENT STANDARDS

*	Language: C99 only.
*	Style: OpenBSD KNF.
*	Documentation: Doxygen for all exported symbols.
*	Principle: small reusable functions, strict module ownership, minimal coupling.
*	File size: soft limit 350 LOC, review trigger 500 LOC.
*	Function size: soft limit 40 LOC, review trigger 80 LOC.
*	Return convention:
	**0**
	success /
	**-1**
	failure unless documented otherwise.
*	TLS: not implemented &#8212; run behind
	relayd(8)
	for TLS termination.

# SEE ALSO

kqueue(2),
pledge(2),
unveil(2),
sysctl(3),
getifaddrs(3),
getmntinfo(3),
mandoc(1),
pkg\_info(1),
relayd(8)

# AUTHORS

The MiniWeb contributors.
See the source code repository for a complete list.

# BUGS

Please report bugs to the issue tracker at
[https://github.com/flavioferretti/miniweb/issues](https://github.com/flavioferretti/miniweb/issues).

OpenBSD - February 25, 2026 - MINIWEB(1)
