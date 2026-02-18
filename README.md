# MiniWeb - OpenBSD System Monitoring Web Server

[![OpenBSD](https://img.shields.io/badge/OpenBSD-7.8-orange.svg)](https://www.openbsd.org/)
[![License](https://img.shields.io/badge/license-BSD%203--Clause-blue.svg)](LICENSE)
[![C](https://img.shields.io/badge/language-C99-brightgreen.svg)](https://en.wikipedia.org/wiki/C99)

A lightweight, secure HTTP server written in C for OpenBSD, built on [OpenBSD](https://www.openbsd.org). Designed for system monitoring, metrics collection, and manual page browsing with OpenBSD's security-first approach.

![Dashboard Screenshot](docs/screenshot.png)

---

## ✨ Features

- **📊 Real-time System Metrics**: CPU, memory, swap, load average, disk usage, network interfaces
- **🔍 Process Monitoring**: Top processes by CPU and memory usage with live updates
- **📚 Man Page Browser**: Search and render OpenBSD manual pages in HTML
- **🎨 Modern Dashboard**: Clean, responsive web interface with tabbed navigation
- **🔒 Security Hardened**: Uses OpenBSD's `pledge()` and `unveil()` for sandboxing
- **⚡ High Performance**: Multi-threaded with connection pooling and caching
- **📡 RESTful API**: JSON endpoints for easy integration with monitoring tools

---

## 🚀 Quick Start

### Prerequisites

- OpenBSD 7.8 or later
- `gnuplot` installed: `pkg_add gnuplot` #Optional used in `benchmark.sh`
- `wrk` installed: `pkg_add wrk` #Optional used in `benchmark.sh`
- Standard OpenBSD development tools (`clang`, `make`)

### Build

```bash
make clean && make
```

## Build documentation

```bash
make man
```

## Build unit-tests
```bash
make unit-tests
```

### Run

```bash
./build/miniweb -v
```

Then open http://127.0.0.1:9001 in your browser.

---

## 📖 Usage

### Command Line Options

```bash
./build/miniweb [options]

Options:
  -p PORT      Port to listen on (default: 9001)
  -b ADDR      Address to bind to (default: 127.0.0.1)
  -t NUM       Thread pool size (default: 4)
  -c NUM       Max connections (default: 1000)
  -v           Enable verbose output
  -h           Show this help
```

### Examples

```bash
# Start on port 8080 with verbose logging
./build/miniweb -p 8080 -v

# Bind to all interfaces with 8 threads
./build/miniweb -b 0.0.0.0 -t 8

# Production mode with high connection limit
./build/miniweb -b 127.0.0.1 -c 5000
```

---

## 🌐 HTTP Endpoints

### Web Interface

| Endpoint | Description |
|----------|-------------|
| `GET /` | Main dashboard with system overview |
| `GET /docs` | Manual page browser and search |
| `GET /networking` | Network interfaces overview with routes and DNS |
| `GET /apiroot` | MiniWeb API root page |

### Metrics API

| Endpoint | Description | Response Format |
|----------|-------------|-----------------|
| `GET /api/metrics` | Complete system metrics | JSON |

**Metrics included:**
- System info (hostname, OS, uptime)
- CPU load averages (1min, 5min, 15min)
- Memory stats (total, free, active, wired, cache)
- Swap usage
- Disk usage for all mounted filesystems
- Network interfaces (IP, status)
- Top 10 processes by CPU usage
- Top 10 processes by memory usage
- Process statistics (total, running, sleeping, zombie)

### Manual Pages API

| Endpoint | Description | Format |
|----------|-------------|--------|
| `GET /api/man/sections` | List all man sections | JSON |
| `GET /api/man/pages?section=X` | List pages in section X | JSON |
| `GET /api/man/search?q=query` | Search manual pages | TEXT |
| `GET /man/{area}/{section}/{page}` | Render specific man page | JSON |

### Static Files

| Endpoint | Description |
|----------|-------------|
| `GET /static/*` | Serve static assets (CSS, JS, images) |
| `GET /favicon.ico` | Site favicon |

---

## 🏗️ Architecture

### Project Structure

```
|-- Makefile                # BSD make
|-- README.md
|-- benchmark.sh            # Simple benchmark
|-- build/                  # Compiled binaries
|-- docs/                   # Documentation
|-- include
|   |-- config.h
|   |-- http_handler.h
|   |-- http_utils.h
|   |-- man.h
|   |-- metrics.h
|   |-- networking.h
|   |-- routes.h
|   |-- template_engine.h
|   `-- urls.h
|-- src
|   |-- http_handler.c
|   |-- http_utils.c
|   |-- main.c
|   |-- man.c
|   |-- metrics.c
|   |-- networking.c
|   |-- routes.c
|   |-- template_engine.c
|   `-- urls.c
|-- static/                 # CSS, JavaScript, images
|-- templates/              # HTML templates
`-- tests
    |-- integration_endpoints.sh
    |-- routes_test.c
    `-- template_test.c

```

### Key Components

#### Metrics Collection (`metrics.c`)

Uses OpenBSD's `sysctl(2)` API to gather system information without spawning external processes:

- **Memory**: `sysctl(CTL_VM, VM_UVMEXP)` for UVM statistics
- **Load**: `getloadavg(3)` for CPU load averages
- **Disks**: `getmntinfo(3)` for filesystem usage
- **Processes**: `sysctl(KERN_PROC, KERN_PROC_ALL)` for process list
- **Network**: `getifaddrs(3)` for interface information

**Critical Implementation Details**: 

1. **Process List Retry Loop**: Uses progressive buffer allocation (25%, 50%, 75%, 100% extra space) to handle the race condition where processes spawn between size query and data retrieval. Each request allocates ~130-160 elements with the `mib[5]` parameter set to the requested element count.

2. **Thread Safety**: All metric collection functions use thread-safe variants:
   - `localtime_r()` instead of `localtime()` for timestamp generation
   - `getpwuid_r()` instead of `getpwuid()` for username lookups
   - Dynamic memory allocation per request (via `malloc()`) with automatic cleanup by libmicrohttpd (`MHD_RESPMEM_MUST_FREE`)

3. **Performance**: Each JSON buffer is allocated dynamically (~8KB per request) and freed automatically by libmicrohttpd after transmission, eliminating lock contention and enabling full parallel processing across all worker threads.

#### Security (`main.c`)

Implements defense-in-depth with OpenBSD's security features:

**unveil(2)** - Restricts filesystem access:
```c
unveil("templates", "r");
unveil("static", "r");
unveil("/usr/share/man", "r");
unveil("/etc/passwd", "r");     // For getpwuid()
unveil("/etc/group", "r");      // For process user names
unveil(NULL, NULL);             // Lock it down
```

**pledge(2)** - Restricts system calls:
```c
pledge("stdio rpath inet proc exec vminfo ps getpw", NULL);
```

Required promises:
- `stdio`: Basic I/O operations
- `rpath`: Read files (templates, static)
- `inet`: Network operations (HTTP server)
- `proc`, `exec`: For man page rendering (mandoc)
- `vminfo`: Memory statistics via sysctl
- `ps`: Process information via sysctl
- `getpw`: User/group name lookups

#### Template Engine (`template_engine.c`)

Simple placeholder-based templating:
```html
<!-- In template -->
<title>{{TITLE}}</title>
<main>{{CONTENT}}</main>

<!-- Replaced at runtime -->
<title>System Dashboard</title>
<main>... rendered content ...</main>
```

---

## 🔒 Security Considerations

### Hardening Features

✅ **OpenBSD Sandboxing**: `pledge()` and `unveil()` restrict capabilities  
✅ **Path Traversal Protection**: Rejects `..` and `//` in static file paths  
✅ **Input Validation**: Sanitizes user input in search queries  
✅ **Safe Process Execution**: Controlled subprocess invocation for mandoc  
✅ **Connection Limits**: Prevents resource exhaustion attacks  
✅ **No Shell Injection**: Direct syscalls instead of shell commands for metrics

### Known Limitations

⚠️ **Manual Page Rendering**: Uses `popen()` to invoke `mandoc` - isolated via pledge/unveil  
⚠️ **JSON Construction**: Static buffers with fixed size limits  
⚠️ **No HTTPS**: Intended for localhost or behind reverse proxy with TLS  
⚠️ **No Authentication**: Deploy behind authenticated proxy if exposed

### Production Deployment

For production use, run behind a reverse proxy with:
- TLS termination (relayd, nginx)
- Authentication (HTTP basic auth, OAuth2)
- Rate limiting
- Request filtering

#### OpenBSD relayd Configuration

Example relayd configuration for HTTPS termination:

```
# /etc/relayd.conf

# Define backend server
table <miniweb> { 127.0.0.1 }

# HTTPS protocol with headers
http protocol "miniweb_https" {
    tls keypair "yourdomain.com"
    
    # Forward all requests to miniweb
    match request path "/*" forward to <miniweb>
    
    # Add forwarding headers
    match request header append "X-Forwarded-For" value "$REMOTE_ADDR"
    match request header append "X-Forwarded-Proto" value "https"
    match request header append "X-Real-IP" value "$REMOTE_ADDR"
    match request header set "Connection" value "close"
}

# Relay configuration
relay "miniweb_relay" {
    # Listen on external interface with TLS
    listen on 0.0.0.0 port 443 tls
    protocol miniweb_https
    
    # Forward to miniweb on localhost
    forward to <miniweb> port 9001 check tcp
}
```

Generate TLS certificate:
```bash
# Self-signed for testing
openssl req -x509 -newkey rsa:4096 -nodes \
    -keyout /etc/ssl/private/yourdomain.com.key \
    -out /etc/ssl/yourdomain.com.crt \
    -days 365 -subj "/CN=yourdomain.com"

# Let's Encrypt with acme-client
acme-client yourdomain.com
```

Enable and start relayd:
```bash
# Enable at boot
rcctl enable relayd

# Start service
rcctl start relayd

# Check status
rcctl check relayd
relayctl show summary
```


---

## 🛠️ Development

### Code Organization

- **Modular Design**: Each component is self-contained with clear interfaces
- **Error Handling**: Comprehensive error checking with graceful degradation
- **Logging**: Configurable verbose mode for debugging
- **Memory Safety**: Careful buffer management and bounds checking

### Building from Source

```bash
# Clean build
make clean && make

# Debug build with symbols
make DEBUG=1

# Run with verbose logging
./build/miniweb -v
```

### Testing

```bash
# Test metrics endpoint
curl -s http://127.0.0.1:9001/api/metrics | jq .

# Test man page search
curl -s 'http://127.0.0.1:9001/api/man/search?q=pledge' | jq .

# Load test
ab -n 1000 -c 10 http://127.0.0.1:9001/
```

---

## 📊 Performance

### Benchmarks

On OpenBSD 7.8 / AMD64 / 4-core CPU:

- **Metrics endpoint**: 250-350 req/sec (includes sysctl + JSON generation)
- **Static files**: ~2000 req/sec
- **Memory footprint**: ~8-12 MB resident
- **CPU usage**: <1% idle, ~50-80% under heavy load (4 threads active)

### Optimization Notes

- Dynamic memory allocation per request eliminates lock contention
- Thread-safe functions (`localtime_r()`, `getpwuid_r()`) enable full parallelism
- Process list retrieval uses efficient sysctl interface with retry loop (no fork/exec)
- libmicrohttpd handles connection pooling and automatic memory cleanup
- Multi-threaded request handling with 4 worker threads (default)

---

## 🐛 Troubleshooting

### Common Issues

**Problem**: `sysctl data query failed: Cannot allocate memory`

**Solution**: This is handled by the retry loop with progressive buffer allocation (up to 100% extra space). The code sets `mib[5]` to the requested element count and retries with larger buffers. If it still persists after 4 retries, your system might have extremely high process churn.

**Problem**: `pledge: Operation not permitted`

**Solution**: Ensure all required promises are in the pledge string. Required: `stdio rpath inet proc exec vminfo ps getpw`. The `ps` promise enables process metrics, and `getpw` enables username lookups via `/etc/passwd`.

**Problem**: Segmentation fault under load

**Solution**: Ensure you're using the thread-safe versions of functions:
- `localtime_r()` instead of `localtime()` 
- `getpwuid_r()` instead of `getpwuid()`
- Dynamic allocation with `MHD_RESPMEM_MUST_FREE` instead of static buffers

These changes are required for multi-threaded operation with libmicrohttpd.

**Problem**: Process data is empty (`top_cpu_processes: []`)

**Solutions**:
1. Verify `ps` and `getpw` promises are in pledge
2. Ensure `/etc/passwd` and `/etc/group` are unveiled
3. Check verbose logs with `-v` flag

**Problem**: Man pages not rendering

**Solution**: Ensure `mandoc` is installed and unveiled paths include `/usr/bin/mandoc` and man directories.

---

## 🗺️ Roadmap

### Planned Features

- [ ] WebSocket support for live metric streaming
- [ ] Historical data collection and graphing
- [ ] Alert thresholds and notifications
- [ ] Plugin system for custom metrics
- [ ] IPv6 support
- [ ] Prometheus exporter compatibility
- [ ] Docker/container support

### Technical Debt

- [ ] Replace remaining `popen()` calls with safer alternatives
- [x] Add comprehensive test suite
- [ ] Improve JSON generation (streaming, proper escaping)
- [ ] Add configuration file support
- [ ] Port to other BSD systems (FreeBSD, NetBSD)
- [x] ~~Implement thread-safe metric collection~~ ✓ Completed
- [x] ~~Fix KERN_PROC_ALL memory allocation issues~~ ✓ Completed
- [x] ~~Replace static buffers with dynamic allocation~~ ✓ Completed

---

## 📝 License

BSD 3-Clause License - See [LICENSE](LICENSE) file for details.

---

## 🙏 Acknowledgments

- Inspired by OpenBSD's security-first philosophy
- Uses OpenBSD's excellent system APIs and documentation

---

# SECURITY CONSIDERATIONS

**miniweb**
implements defense-in-depth security using OpenBSD's security features:

*	pledge(2)
	restricts the process to only necessary system calls.

*	unveil(2)
	limits filesystem access to required paths only.

*	Path traversal protection rejects attempts to access files outside
	*/static*.

*	Process metrics use
	sysctl(2)
	directly instead of spawning
	ps(1),
	eliminating shell injection risks.

*	Connection limits prevent resource exhaustion attacks.

For production use, deploy behind an authenticated reverse proxy with
TLS, rate limiting, and request filtering.
Never expose
**miniweb**
directly to untrusted networks without additional security controls.

# BUGS

The JSON generation uses fixed-size buffers and may truncate output
if metrics exceed buffer capacity.

The template engine performs simple string replacement without proper
escaping, which could allow HTML injection if user input is rendered
in templates.
Current implementation only uses trusted input.

Subprocess timeout handling for
mandoc(1)
invocation is not fully implemented, which could cause hangs if
mandoc(1)
stalls.

# SEE ALSO

apropos(1),
mandoc(1),
kqueue(2),
pledge(2),
unveil(2)

# HISTORY

**miniweb**
originally used libmicrohttpd but was rewritten in 2026 to use a native
kqueue engine for better integration with OpenBSD.

OpenBSD 7.8 - February 14, 2026 - MINIWEB(1)
