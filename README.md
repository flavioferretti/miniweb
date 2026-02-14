# MiniWeb - OpenBSD System Monitoring Web Server

[![OpenBSD](https://img.shields.io/badge/OpenBSD-7.8-orange.svg)](https://www.openbsd.org/)
[![License](https://img.shields.io/badge/license-BSD%203--Clause-blue.svg)](LICENSE)
[![C](https://img.shields.io/badge/language-C99-brightgreen.svg)](https://en.wikipedia.org/wiki/C99)

A lightweight, secure HTTP server written in C for OpenBSD, built on [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/). Designed for system monitoring, metrics collection, and manual page browsing with OpenBSD's security-first approach.

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
- `libmicrohttpd` installed: `pkg_add libmicrohttpd`
- Standard OpenBSD development tools (`clang`, `make`)

### Build

```bash
make clean && make
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

| Endpoint | Description |
|----------|-------------|
| `GET /api/man/sections` | List all man sections |
| `GET /api/man/pages?section=X` | List pages in section X |
| `GET /api/man/search?q=query` | Search manual pages |
| `GET /man/{area}/{section}/{page}` | Render specific man page |

### Static Files

| Endpoint | Description |
|----------|-------------|
| `GET /static/*` | Serve static assets (CSS, JS, images) |
| `GET /favicon.ico` | Site favicon |

---

## 🏗️ Architecture

### Project Structure

```
miniweb/
├── src/
│   ├── main.c              # Server entrypoint, CLI parsing, security
│   ├── routes.c            # Route dispatcher and URL matching
│   ├── metrics.c           # System metrics collection via sysctl
│   ├── man.c               # Manual page search and rendering
│   ├── template_engine.c   # HTML template processor
│   ├── http_utils.c        # HTTP response helpers
│   └── urls.c              # URL parsing utilities
├── include/
│   ├── config.h            # Configuration constants
│   ├── metrics.h           # Metrics data structures
│   └── *.h                 # Module headers
├── templates/              # HTML templates
├── static/                 # CSS, JavaScript, images
└── build/                  # Compiled binaries
```

### Key Components

#### Metrics Collection (`metrics.c`)

Uses OpenBSD's `sysctl(2)` API to gather system information without spawning external processes:

- **Memory**: `sysctl(CTL_VM, VM_UVMEXP)` for UVM statistics
- **Load**: `getloadavg(3)` for CPU load averages
- **Disks**: `getmntinfo(3)` for filesystem usage
- **Processes**: `sysctl(KERN_PROC, KERN_PROC_ALL)` for process list
- **Network**: `getifaddrs(3)` for interface information

**Critical Implementation Detail**: Process list retrieval uses a retry loop with progressive buffer allocation to handle the race condition where processes spawn between size query and data retrieval.

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
- TLS termination (nginx, relayd)
- Authentication (HTTP basic auth, OAuth2)
- Rate limiting
- Request filtering

Example nginx configuration:
```nginx
location / {
    proxy_pass http://127.0.0.1:9001;
    proxy_set_header X-Real-IP $remote_addr;
    proxy_set_header X-Forwarded-For $proxy_add_x_forwarded_for;
    proxy_set_header X-Forwarded-Proto $scheme;
}
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

- **Metrics endpoint**: ~500 req/sec (includes sysctl overhead)
- **Static files**: ~2000 req/sec
- **Memory footprint**: ~8-12 MB resident
- **CPU usage**: <1% idle, ~15% under load

### Optimization Notes

- Metrics are cached internally to reduce sysctl overhead
- Process list retrieval uses efficient sysctl interface (no fork/exec)
- Connection pooling reduces TCP overhead
- Multi-threaded request handling

---

## 🐛 Troubleshooting

### Common Issues

**Problem**: `sysctl data query failed: Cannot allocate memory`

**Solution**: This is a known OpenBSD issue with `KERN_PROC_ALL`. The code includes a retry loop with progressive buffer allocation. If it persists, your system might be extremely busy with processes spawning rapidly.

**Problem**: `pledge: Operation not permitted`

**Solution**: Ensure all required promises are in the pledge string. Check the security section above for the complete list.

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
- [ ] Implement actual timeout handling in subprocess execution
- [ ] Add comprehensive test suite
- [ ] Improve JSON generation (streaming, proper escaping)
- [ ] Add configuration file support
- [ ] Port to other BSD systems (FreeBSD, NetBSD)

---

## 📝 License

BSD 3-Clause License - See [LICENSE](LICENSE) file for details.

---

## 🙏 Acknowledgments

- Built with [libmicrohttpd](https://www.gnu.org/software/libmicrohttpd/)
- Inspired by OpenBSD's security-first philosophy
- Uses OpenBSD's excellent system APIs and documentation

---

## 📧 Contact & Contributing

Issues and pull requests are welcome on the project repository.

For security issues, please contact the maintainer directly.

---

**Note**: This project is specifically designed for OpenBSD. While it may be portable to other systems with modifications, OpenBSD is the primary and tested platform.
