MINIWEB(1) - General Commands Manual

# NAME

**miniweb** - OpenBSD-native C99 HTTP server

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
Three independent caching layers improve performance:

-	Template-backed HTML views are served from an in-memory template cache
	(refreshed every 60 seconds).
-	Static assets are served from a sharded file cache with admission policy.
-	Manual pages use a two-level cache (L1: RAM, L2: filesystem).

A generic heartbeat scheduler fires periodic background tasks for metrics
collection and cache maintenance.

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

# CACHING

**miniweb**
maintains three independent in-process caches.

Template cache

> All files in
> **templates\_dir**
> are preloaded at startup and refreshed every 60 seconds.
> A mutex-protected directory scan replaces the full cache on each refresh.

Static file cache

> Sharded LRU with 16 shards, 32 slots per shard, a 256 KiB per-file
> limit, a 120-second TTL, and an admission policy requiring 2 hits before
> a file is promoted.
> Insert rate is capped at 8 entries per shard per second.

Man render cache

> Two-level.
> *L1*
> is an in-process sharded cache: 8 shards, 64 slots per shard, 600-second TTL.
> Entries are malloc'd copies of the rendered body.
> *L2*
> is a filesystem cache under
> *static/man/{area}/{section}/{page}.{format}*
> with a 300-second TTL.
> On a full miss, mandoc is invoked as a subprocess.
> Cache invalidation requires a server restart or TTL expiry.

# MODULE LAYOUT STATUS

Current module layout in
*src/modules*
reflects the ongoing refactor plan:

-	Manual pages: facade + split internals
	(*man\_module.c*, *man\_service.c*, *man\_json.c*, *man\_query.c*, *man\_index.c*, *man\_render.c*).
-	Metrics: orchestrator + collectors + snapshots + process/json units
	(*metrics\_module.c*, *metrics\_collectors.c*, *metrics\_snapshot.c*, *metrics\_process.c*, *metrics\_json.c*).
-	Networking and packages still keep larger orchestrator units and are next extraction targets.

For the manual pages stack,
*man\_module.c*
is now route wiring and cleanup delegation only;
query validation/parsing lives in
*man\_query.c*,
index/search preparation lives in
*man\_index.c*,
and HTTP payload shaping/render caching lives in
*man\_render.c*.
*man\_service.c*
remains the service-facing facade.

# RELIABILITY NOTES

Recent hardening updates include:

-	Worker request dispatch now executes each matched handler exactly once per
	request.
-	Idle-connection sweeping uses synchronized pool inspection to avoid dispatcher
	vs worker races under high load.
-	HTTP response header assembly clamps/truncates safely and rejects overlong
	header blocks instead of performing unsafe pointer arithmetic.
-	Server startup now uses unified cleanup paths so listen/kqueue descriptors are
	closed on all initialization failures.
-	Forwarded client-IP and HTTPS proxy headers are only trusted when the socket
	peer IP matches
	**trusted\_proxy**.
-	Shutdown performs explicit cleanup of process-global HTTP/man caches and man
	module semaphore resources.

# PERFORMANCE

For best throughput and tail latency, tune worker and cache settings together.

Recommended tuning order:

1.	Increase
	**threads**
	incrementally while observing latency and CPU saturation.
2.	Set
	**max\_conns**
	below host descriptor/memory limits to avoid overload collapse.
3.	Prefer cache-friendly assets and templates:

	*	Template cache and hot-view cache reduce repeated render costs.
	*	Sharded static file cache reduces disk I/O for hot assets.
	*	Metrics/networking periodic snapshots avoid expensive rebuild on every request.
	*	Packages and man endpoints use query/result caches to reduce subprocess load.

4.	Move periodic collectors into heartbeat tasks with explicit periods.
5.	When SQLite backend is enabled, reuse prepared statements and group writes in
	transactions to reduce fsync overhead.

Current SQLite facade functions are:
**mw\_db\_open**(),
**mw\_db\_exec\_schema**(),
**mw\_stmt\_prepare**(),
**mw\_stmt\_step**(),
and transaction helpers
**mw\_tx\_begin**(*/*)
**mw\_tx\_commit**(*/*)
**mw\_tx\_rollback**().
These APIs are designed for migration of feature flags, warm caches, and
historical snapshots.

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
on non-blocking sockets with a limited retry count (5 attempts, 100 ms
polling) to prevent CPU spinning on full send buffers.

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
A semaphore limits concurrent mandoc subprocesses to
*min(threads \* 2, 16)*
to prevent file descriptor exhaustion under heavy load.

# ROUTING

Routes are stored in a flat array (up to
`MAX_ROUTES`
&equals; 32 entries) registered at startup by
**init\_routes**()
in
*src/router/url\_registry.c*.

**route\_match**()
resolves a handler in two passes:

1.	Exact match: linear scan comparing method and path strings.

2.	Dynamic prefix match (GET only):

	*	*/man/{area}/{section}/{page}\[.fmt]*
		&#8212; matched when the path begins with
		*/man/*
		and contains at least two additional slashes.
	*	*/api/man/...*
		&#8212; prefix match.
	*	*/api/packages/...*
		&#8212; prefix match.
	*	*/static/...*
		&#8212; prefix match.

When a path is known but the method is wrong, a
**405 Method Not Allowed**
response is sent with an
`Allow`
header.
Unknown paths receive
**404 Not Found**.

# ADDING AND REMOVING ROUTES, MODULES, AND WEB VIEWS

## Adding an API endpoint

Implement an
**attach\_routes**()
callback in the new module and register routes via the router facade:

	int my_module_attach_routes(struct router *r)
	{
	    return router_register(r, "GET", "/api/myfeature",
	        my_feature_handler);
	}

Add the module descriptor to the
*miniweb\_module*
array in
**init\_routes**()
inside
*src/router/url\_registry.c*
with
*enabled\_by\_default*
set to 1.
The handler must have the signature
*int* **handler**(*http\_request\_t \*req*)
and call one of the HTTP send helpers before returning.
Raise
`MAX_ROUTES`
in
*include/miniweb/router/urls.h*
if more than 32 routes are needed.

## Adding a web view

1.	Add an entry to the
	*view\_routes\[]*
	table in
	*src/router/url\_registry.c*:

		{"GET", "/mypage", "MiniWeb - My Page",
		 "mypage.html",
		 "mypage_extra_head.html",
		 "mypage_extra_js.html"},

2.	Create
	*templates/mypage.html*
	with the page body content.
	The base layout
	*templates/base.html*
	is injected automatically.
	The optional fragment files are silently skipped if absent.

3.	The URL is automatically registered by
	**views\_module\_attach\_routes**()
	at startup.
	Add the path to
	*g\_hot\_view\_cache*
	in
	*src/router/route\_table.c*
	if the rendered HTML should be cached.

## Removing a route or view

Remove the
**register\_route**()
call or delete the entry from
*view\_routes\[]*.
Requests to that path will receive
**404**.

## Module attach API

**miniweb\_module\_attach\_enabled**()
in
*src/router/module\_attach.c*
iterates a
*miniweb\_module*
array and calls each enabled module's
**init**()
and
**attach\_routes**()
callbacks.
All built-in modules
(metrics, networking, man, packages, and views)
are registered through this contract at startup.

# TEMPLATE ENGINE AND VIEW CACHE

## Template engine

All HTML files in
**templates\_dir**
are preloaded into a heap-allocated in-memory cache at startup by
**template\_cache\_init**().
The cache is refreshed lazily every 60 seconds.

**template\_render\_with\_data**()
assembles a page by loading
*base.html*,
the page-specific content file, and optional
*extra\_head*
and
*extra\_js*
fragments, then substituting the placeholders
'`{{title}}`',
'`{{page_content}}`',
'`{{extra_head}}`',
and
'`{{extra_js}}`'.

## Hot view cache

**view\_template\_handler**()
maintains a small cache of pre-rendered HTML for the five top-level pages
(*/*, */docs*, */networking*, */packages*, */apiroot*).
Entries are valid for 10 seconds.
On a cache hit the rendered HTML is duplicated and returned without touching
the template engine.

# STATIC FILE CACHE

Static assets are served from a sharded in-memory cache:

*	16 independent shards, each protecting 32 slots, keyed by file path.
*	Files larger than 256 KiB are never cached and are always streamed.
*	A two-stage admission policy requires at least two requests before a file
	is admitted to the cache.
*	Each shard limits insertions to 8 per second using a token bucket.
*	Entries older than 120 seconds are evicted on the next shard access.
*	Cache hits are validated against
	`st_mtime`.

# HEARTBEAT SCHEDULER

*src/core/heartbeat.c*
provides a generic periodic task scheduler using
`CLOCK_MONOTONIC`
to avoid clock skew issues from NTP adjustments.
Up to 32 named tasks can be registered with
**heartbeat\_register**().
Each task specifies a callback, an opaque context pointer, a period in
seconds, and an initial delay.
**heartbeat\_register**()
returns
`HB_REGISTER_INSERTED`
(1) on success,
`HB_REGISTER_DUPLICATE`
(0) for a duplicate name (non-fatal), and
`HB_REGISTER_ERROR`
(-1) on invalid input or a full table.
**heartbeat\_start**()
spawns a single background thread that fires due tasks and tracks overrun
counts per task.
**heartbeat\_shutdown**() *drain*
stops the scheduler; when
*drain*
is non-zero, every active task is executed one final time before the thread
exits.
Per-task statistics (runs, overruns, last run, last error) are available via
**heartbeat\_get\_stats**().

Both the metrics and networking modules are fully migrated onto the heartbeat
scheduler.
Each module registers a named task
("metrics.sample and "networking.sample"")
and drives its 1-second sampler through it.

# MODULES

All modules are registered at startup through
**miniweb\_module\_attach\_enabled**()
in
*src/router/url\_registry.c*.
Each module provides an
**attach\_routes**()
callback that registers its endpoints through the
*struct router*
facade, and an optional
**init**()
callback invoked first.

## Metrics

Provides
*/api/metrics*
and the
*/*
dashboard.
Collects CPU percentages
(`KERN_CPTIME`),
memory and swap
(`vm.uvmexp`),
load averages
(`vm.loadavg`),
OS info, uptime, hostname, disk usage
(getmntinfo(3)),
and top processes by CPU and RSS via
`KERN_PROC_ALL`
sysctl.
A heartbeat task
("metrics.sample")
samples every second, pushes into a ring buffer, and updates a cached JSON
snapshot; handlers serve the snapshot rather than re-collecting on each request.

## Networking

Provides
*/api/networking*
and the
*/networking*
view.
Collects routing table entries via
`NET_RT_DUMP`,
DNS configuration from
*/etc/resolv.conf*,
and per-interface statistics via
getifaddrs(3) /
*if\_data*.
Active TCP/UDP connection enumeration is currently a placeholder.
A heartbeat task
("networking.sample")
samples every second and maintains a cached JSON snapshot to keep sysctl
traversal off the HTTP request path.

## Manual pages

Provides
*/man/{area}/{section}/{page}\[.fmt]*
and the
*/api/man/...*
namespace.
Renders man pages by forking
mandoc(1)
via
**safe\_popen\_read\_argv**()
with the configured timeout.
A semaphore limits concurrent mandoc subprocesses to prevent file descriptor
exhaustion.
Rendered output is cached in a two-level system:
*L1 cache*
(8 shards, 64 slots, 600-second TTL) in RAM, and
*L2 cache*
on the filesystem under
*{static\_dir}/man/{area}/{section}/{page}.{fmt}*
with a 300-second TTL.
Supported output formats:
**html**, **pdf**, **ps**, **md**, **txt**.
(Note:
**utf8**
is not supported; use
**txt**
for plain text output.)

## Packages

Provides
*/api/packages/...*
and the
*/packages*
view.
Wraps
pkg\_info(1)
to implement search, info, which-file, file-list, and installed-list queries.
Results are cached in a ring buffer with a 30-second TTL.

# STORAGE LAYER

*src/storage/*
contains stub implementations of a planned SQLite3 service layer.
The interfaces are defined but not yet backed by SQLite3:

*	**mw\_db\_open**(), mw\_db\_close
	&#8212; database lifecycle (now stores the provided path).
*	**mw\_db\_exec\_schema**()
	&#8212; schema initialisation.
*	**mw\_db\_migrate**(), mw\_tx\_begin, mw\_tx\_commit, mw\_tx\_rollback
	&#8212; migration and transaction control.
*	**mw\_stmt\_prepare**(),
	**mw\_bind\_text**(),
	**mw\_bind\_int64**(),
	**mw\_bind\_null**(),
	**mw\_stmt\_step**(),
	**mw\_stmt\_finalize**()
	&#8212; prepared statement lifecycle.

All stubs currently return
`-1`
except for
**mw\_db\_open**()
which now properly stores the database path.
The interfaces are stable and intended to be implemented during the Phase 5
storage refactor.

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
	*/usr/bin/man*, */usr/bin/apropos*, */usr/bin/netstat*, */bin/sh*
	(execute).
*	*/usr/sbin/pkg\_info*
	(execute),
	*/var/db/pkg*, */usr/local*, */usr/bin*, */usr/sbin*, */bin*, */sbin*,
	*/usr/local/bin*
	(read).
*	*/etc/passwd*, */etc/group*, */etc/resolv.conf*
	(read),
	*/dev/null*
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

# ENDPOINTS

## Views

*/*

> Dashboard.

*/docs*

> Documentation and man page browser UI.

*/networking*

> Networking UI.

*/packages*

> Package Manager UI.

*/apiroot*

> API Index UI.

## JSON APIs

The following API endpoints are available:

*/api/metrics*

> System metrics snapshot (CPU, RAM, load, disk, processes).

*/api/networking*

> Networking diagnostics (routes, DNS, interfaces).

*/api/man/sections*

> List all available manual sections by area.

*/api/man/pages?section=X&area=Y*

> List pages in a specific section and area.

*/api/man/resolve?name=X&section=Y*

> Resolve a manual page to its filesystem path.

*/api/man/search?q=QUERY*

> Search manual pages.

*/api/packages/search?q=QUERY*

> Search installed packages.

*/api/packages/info?name=PKG*

> Package details.

*/api/packages/which?path=FILE*

> Which package owns a file.

*/api/packages/files?name=PKG*

> Files installed by a package.

*/api/packages/list*

> List all installed packages (sorted lexicographically).

## Manual page rendering

*/man/{area}/{section}/{page}*

> Default HTML rendering.

*/man/{area}/{section}/{page}.html*

> HTML.

*/man/{area}/{section}/{page}.txt*

> Plain text.

*/man/{area}/{section}/{page}.md*

> Markdown.

*/man/{area}/{section}/{page}.pdf*

> PDF.

*/man/{area}/{section}/{page}.ps*

> PostScript.

## Static assets

*/static/\*&zwnj;*

> CSS, JavaScript, images, and other assets.

*/favicon.ico*

> Favicon (served from
> *static/assets/favicon.svg*).

# SOURCE LAYOUT

*src/app\_main.c*

> Server startup, signal handling, configuration loading, and module
> and route registration.
> The kqueue dispatcher and accept loop live in
> *src/net/server.c*.

*src/net/server.c*

> kqueue loop, worker pool, accept, idle sweep, shutdown.

*src/net/work\_queue.c*

> Thread-safe FIFO transport work queue extracted from the server entrypoint.

*src/platform/openbsd/security.c*

> OpenBSD sandbox boundary setup using
> unveil(2)
> and
> pledge(2).

*src/core/conf.c*

> Configuration file parser and CLI override logic.

*src/core/heartbeat.c*

> Periodic task scheduler (up to 32 named tasks, overrun tracking) using
> CLOCK\_MONOTONIC to avoid NTP skew.

*src/core/log.c*

> Thread-safe logger.

*src/http/response.c*

> Response pool, serialisation, sharded file cache, HTTP send helpers.

*src/http/utils.c*

> JSON escaping (fixed buffer overrun), subprocess execution (fork / poll / timeout).

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
> All built-in modules are attached here through the module attach API.

*src/modules/man/*

> Man page rendering, two-level render cache (L1: RAM, L2: filesystem), JSON API.
> Semaphore limits concurrent mandoc subprocesses.

*src/modules/metrics/*

> System metrics collection, heartbeat task, ring buffer, and JSON API.

*src/modules/networking/*

> Networking diagnostics, heartbeat task, ring buffer, and JSON API.

*src/modules/packages/*

> pkg\_info(1)
> wrapper and JSON API with ring buffer caching.

*src/render/template\_render.c*

> Template file cache (preloaded at startup, refreshed every 60 s) and
> placeholder substitution.

*src/storage/sqlite\_db.c*

> SQLite3 database lifecycle stubs (now stores the provided path).

*src/storage/sqlite\_schema.c*

> Schema migration and transaction stubs.

*src/storage/sqlite\_stmt.c*

> Prepared statement stubs.

# SIGNAL HANDLER SAFETY

The server's
*running*
flag is declared as
*volatile sig\_atomic\_t*
to ensure safe access from signal handler context.
This prevents undefined behaviour when
**handle\_signal**()
writes to the flag during asynchronous signal delivery.

# ENTERPRISE REFACTOR ROADMAP

Phase 1

> Module attach API and router separation.
> Complete: all modules register through
> **miniweb\_module\_attach\_enabled**().

Phase 2

> Single global heartbeat scheduler.
> Complete: metrics and networking both run as named heartbeat tasks.

Phase 3

> Domain decomposition of metrics, networking, man, packages.
> Scaffolded:
> *\*\_service.c*
> and
> *\*\_json.c*
> stubs are present; function bodies still to be migrated.

Phase 4

> Documentation and architecture transition.
> Complete: README, man page, and all Doxygen blocks updated.
> Majority Italian-language source comments have been translated to English.

Phase 5

> SQLite3 storage integration.
> Stub interfaces are defined; only basic path storage implemented.

Phase 6

> Performance and observability hardening.
> Planned.

# NEXT STEPS FOR MODULARITY

The following items are the immediate backlog for reaching low
LOC-per-file and low LOC-per-function targets across the codebase.

1.	Migrate function bodies from
	*\*\_module.c*
	into the corresponding
	*\*\_service.c*
	(data collection) and
	*\*\_json.c*
	(JSON serialisation) units for all four feature modules.
	After migration,
	*\*\_module.c*
	should contain only route handler entry points and
	**attach\_routes**().
2.	Split
	*src/http/response.c*
	(~973 LOC) into
	*response\_writer.c*,
	*static\_files.c*,
	and
	*mime.c*.
3.	Extract platform-specific collectors into dedicated translation units under
	*src/platform/openbsd/*
	for CPU, memory, disk, process, network interfaces, and routing table.
	Modules must consume only collector APIs; no direct sysctl or kvm calls
	should remain inside module files.
4.	Apply function-size limits (soft 40 LOC, review trigger 80 LOC) to
	**miniweb\_server\_run**(),
	**man\_render\_handler**(),
	and
	**build\_system\_metrics\_json**().
5.	Upgrade
	*tests/integration\_endpoints.sh*
	from reachability smoke tests to contract-level assertions: explicit
	status checks for 200/404/405 and JSON key-presence checks for core
	endpoints.
6.	Implement the SQLite3 backend (Phase 5): connect
	*src/storage/*
	stubs to real
	**sqlite3\_open\_v2**(),
	prepared statements, and transactions; persist module flags and bounded
	metric snapshots.

# DESIGN REQUIREMENTS

*	C99-only codebase.
*	OpenBSD KNF style.
*	Small, reusable functions with strict module ownership.
*	File-size guideline: soft limit 350 LOC, review trigger 500 LOC.
*	Function-size guideline: soft limit 40 LOC, review trigger 80 LOC.
*	Doxygen documentation for all exported symbols.
*	All source comments in English.
*	Compatibility-first refactor: feature parity required at every phase.

# SEE ALSO

kqueue(2),
pledge(2),
unveil(2),
sysctl(3),
getifaddrs(3),
getmntinfo(3),
mandoc(1),
apropos(1),
pkg\_info(1),
relayd(8)

# AUTHORS

*	Flavio Ferretti &lt;[flavio@flvbox.org](mailto:flavio@flvbox.org)&gt;
*	[https://github.com/flavioferretti/miniweb](https://github.com/flavioferretti/miniweb)

# REFACTOR STATUS

As of 2026-03-01, server runtime decomposition is complete for the
networking layer and documentation is up to date:

*	*src/net/server.c*
	&#8212; kqueue dispatcher and accept loop.

*	*src/net/connection\_pool.c*
	&#8212; fd-indexed connection pool with O(1) alloc/free and generation guards.

*	*src/net/worker.c*
	&#8212; worker request read, parse, and route dispatch.

Module decomposition has started for metrics and scaffolded all feature modules:

*	*src/modules/metrics/metrics\_json.c*
	and
	*src/modules/metrics/metrics\_process.c*
	now own serializer/process extraction logic, while
	*src/modules/metrics/metrics\_module.c*
	keeps orchestration and endpoint wiring.

*	*src/modules/networking/networking\_service.c*
	and
	*src/modules/networking/networking\_json.c*
	remain scaffold units pending full extraction.

*	*src/modules/man/man\_service.c*
	and
	*src/modules/man/man\_json.c*
	remain scaffold units pending full extraction.

*	*src/modules/packages/packages\_service.c*
	and
	*src/modules/packages/packages\_json.c*
	remain scaffold units pending full extraction.

All Doxygen
'TODO'
placeholder blocks have been replaced with accurate descriptions.
Majority Italian-language source comments have been translated to English.
Recent bug fixes include:

*	JSON string escaping buffer overrun corrected.
*	HTTP 405 status text now returns "Method Not Allowed".
*	Signal handler safety improved with volatile sig\_atomic\_t.
*	Mandoc subprocess concurrency limited via semaphore.
*	SQLite database path now properly stored.
*	Heartbeat scheduler uses CLOCK\_MONOTONIC to avoid NTP skew.

OpenBSD - February 28, 2026 - MINIWEB(1)
