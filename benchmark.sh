#!/bin/ksh
#
# benchmark.sh - Build a detailed benchmark report for MiniWeb.
# Generates static/benchmark.html and per-metric SVG graphs in static/benchmark_assets.
#

set -eu
SERVER_PORT="${SERVER_PORT:-3000}"
BASE_URL="http://localhost:${SERVER_PORT}"
TEST_DURATION="${TEST_DURATION:-20}"
THREADS="${THREADS:-4}"
CONNECTIONS="${CONNECTIONS:-4 8 16 32 64 128}"
OUTPUT_ROOT="${OUTPUT_ROOT:-static}"
ASSETS_DIR="${OUTPUT_ROOT}/benchmark_assets"
TIMESTAMP="$(date '+%Y-%m-%d %H:%M:%S')"
CSV_FILE="${ASSETS_DIR}/results.csv"
HTML_FILE="${OUTPUT_ROOT}/benchmark.html"
GNUPLOT_SCRIPT="${ASSETS_DIR}/plot.gp"

# Endpoint definitions
set -A ENDPOINT_IDS -- \
    "STATIC_HTML" "STATIC_CSS" "STATIC_JS" "STATIC_IMG" \
    "PAGE_HOME" "PAGE_DOCS" "PAGE_NETWORKING" "PACKAGES_UI" "API_ROOT" \
    "API_METRICS" "API_NETWORKING" \
    "API_PKG_SEARCH" "API_PKG_INFO" "API_PKG_WHICH" "API_PKG_FILES" "API_PKG_LIST" \
    "MAN_SEARCH" "MAN_HTML" "MAN_HTML_EXT" "MAN_MD" "MAN_PDF" "MAN_PS"

set -A ENDPOINT_URLS -- \
    "/static/test.html" "/static/css/custom.css" "/static/js/theme_toggler.js" "/static/assets/favicon.svg" \
    "/" "/docs" "/networking" "/packages" "/apiroot" \
    "/api/metrics" "/api/networking" \
    "/api/packages/search?q=curl" "/api/packages/info?name=curl" "/api/packages/which?path=/bin/ls" "/api/packages/files?name=curl" "/api/packages/list" \
    "/api/man/search?q=pledge" "/man/1/1/ls" "/man/1/1/ls.html" "/man/1/1/ls.md" "/man/1/1/ls.pdf" "/man/1/1/ls.ps"

set -A ENDPOINT_NAMES -- \
    "Static HTML" "Static CSS" "Static JS" "Static Image" \
    "Home Dashboard" "Documentation" "Networking" "Packages UI" "API Index" \
    "System Metrics API" "Network Info API" \
    "Package Search (curl)" "Package Info (curl)" "Package Owner (/bin/ls)" "Package Files (curl)" "Package List" \
    "Man Search (pledge)" "Man Page (ls, HTML)" "Man Page (ls, .html)" "Man Page (ls, Markdown)" "Man Page (ls, PDF)" "Man Page (ls, PostScript)"

# Helper functions
trim_value() {
  printf '%s' "$1" | sed 's/^[[:space:]]*//;s/[[:space:]]*$//'
}

to_ms() {
  typeset raw num unit
  raw="$(trim_value "$1" | tr -d ',')"
  num="$(printf '%s' "$raw" | sed 's/[^0-9.]//g')"
  unit="$(printf '%s' "$raw" | sed 's/[0-9.]//g')"

  [ -z "$num" ] && num="0"

  case "$unit" in
    us) awk -v n="$num" 'BEGIN { printf "%.3f", n/1000 }' ;;
    ms|'') awk -v n="$num" 'BEGIN { printf "%.3f", n }' ;;
    s) awk -v n="$num" 'BEGIN { printf "%.3f", n*1000 }' ;;
    m) awk -v n="$num" 'BEGIN { printf "%.3f", n*60000 }' ;;
    *) awk -v n="$num" 'BEGIN { printf "%.3f", n }' ;;
  esac
}

to_mbsec() {
  typeset raw num unit
  raw="$(trim_value "$1" | tr -d ',')"
  num="$(printf '%s' "$raw" | sed 's/[^0-9.]//g')"
  unit="$(printf '%s' "$raw" | sed 's/[0-9.]//g')"

  [ -z "$num" ] && num="0"

  case "$unit" in
    B) awk -v n="$num" 'BEGIN { printf "%.3f", n/1048576 }' ;;
    KB) awk -v n="$num" 'BEGIN { printf "%.3f", n/1024 }' ;;
    MB|'') awk -v n="$num" 'BEGIN { printf "%.3f", n }' ;;
    GB) awk -v n="$num" 'BEGIN { printf "%.3f", n*1024 }' ;;
    *) awk -v n="$num" 'BEGIN { printf "%.3f", n }' ;;
  esac
}

# Health check
check_endpoint() {
    typeset url="$1"
    typeset full_url="${BASE_URL}${url}"

    curl -s -G --max-time 10 -o /dev/null -w '%{http_code}' "$full_url" 2>/dev/null || echo "000"
}

# Get HTTP code
get_http_code() {
    typeset url="$1"
    typeset full_url="${BASE_URL}${url}"

    curl -s -G --max-time 3 -o /dev/null -w '%{http_code}' "$full_url" 2>/dev/null || echo "000"
}

# Check dependencies
printf 'MiniWeb Benchmark\n'
printf '=============================================\n'
printf 'Server URL  : %s\n' "$BASE_URL"
printf 'Threads     : %s\n' "$THREADS"
printf 'Duration    : %ss\n' "$TEST_DURATION"
printf 'Connections : %s\n\n' "$CONNECTIONS"

for cmd in wrk gnuplot curl awk sed; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "ERROR: missing dependency '$cmd'" >&2
    exit 1
  fi
done

if ! curl -s --max-time 3 "${BASE_URL}/static/test.html" >/dev/null 2>&1; then
  echo "ERROR: server is unreachable at $BASE_URL" >&2
  exit 1
fi

mkdir -p "$ASSETS_DIR"

cat > "$CSV_FILE" <<'CSV'
connections,req_sec,latency_avg_ms,latency_stdev_ms,latency_max_ms,transfer_mb_sec,total_requests,errors_non_2xx,endpoint_name,endpoint_url,http_code,status
CSV

# Run benchmarks per ogni endpoint
total_endpoints=${#ENDPOINT_IDS[*]}
i=0
while [ $i -lt $total_endpoints ]; do
    id="${ENDPOINT_IDS[$i]}"
    name="${ENDPOINT_NAMES[$i]}"
    url="${ENDPOINT_URLS[$i]}"
    safe_name="$(printf '%s' "$id" | tr '[:upper:]' '[:lower:]')"

    printf '\n▶ Testing: %s\n' "$name"
    printf '  URL: %s\n' "${BASE_URL}${url}"

    # Health check
    printf '    Health check ... '
    http_check=$(check_endpoint "$url")

    case "$http_check" in
        200|201|202|204|301|302|304)
            printf 'OK (HTTP %s)\n' "$http_check"
            ;;
        *)
            printf 'FAILED (HTTP %s) - skipping endpoint\n' "$http_check"
            for conn in $CONNECTIONS; do
                printf '%s,%s,%s,%s,%s,%s,%s,%s,"%s","%s",%s,HEALTH_FAILED\n' \
                    "$conn" "0" "0" "0" "0" "0" "0" "0" "$name" "$url" "$http_check" >> "$CSV_FILE"
            done
            i=$((i + 1))
            sync
            continue
            ;;
    esac

    # Test per ogni livello di connessione
    for conn in $CONNECTIONS; do
        printf '    Connections: %3d ... ' "$conn"

        # Per PDF/PS solo check disponibilità
        case "$url" in
            *.pdf|*.ps)
                http_code=$(get_http_code "$url")
                printf '%s,%s,%s,%s,%s,%s,%s,%s,"%s","%s",%s,BINARY\n' \
                    "$conn" "0" "0" "0" "0" "0" "1" "0" "$name" "$url" "$http_code" >> "$CSV_FILE"
                printf 'OK (binary)\n'
                continue
                ;;
        esac

        # wrk test
        full_url="${BASE_URL}${url}"
        run_output="$(wrk -t"$THREADS" -c"$conn" -d"${TEST_DURATION}"s --latency "$full_url" 2>&1)"

        # Salva output raw
        echo "$run_output" > "${ASSETS_DIR}/${safe_name}_${conn}.txt"

        # Estrai metriche
        req_sec="$(echo "$run_output" | awk '/Requests\/sec:/ {print $2; exit}')"
        [ -z "$req_sec" ] && req_sec="0"

        transfer_raw="$(echo "$run_output" | awk '/Transfer\/sec:/ {print $2; exit}')"
        [ -z "$transfer_raw" ] && transfer_raw="0MB"

        total_requests="$(echo "$run_output" | awk '/requests in/ {print $1; exit}')"
        [ -z "$total_requests" ] && total_requests="0"

        non_2xx="$(echo "$run_output" | awk '/Non-2xx or 3xx responses:/ {print $5; exit}')"
        [ -z "$non_2xx" ] && non_2xx="0"

        latency_line="$(echo "$run_output" | awk '/Latency/ {print; exit}')"
        if [ -n "$latency_line" ]; then
            latency_avg="$(to_ms "$(echo "$latency_line" | awk '{print $2}')")"
            latency_stdev="$(to_ms "$(echo "$latency_line" | awk '{print $3}')")"
            latency_max="$(to_ms "$(echo "$latency_line" | awk '{print $4}')")"
        else
            latency_avg="0"
            latency_stdev="0"
            latency_max="0"
        fi

        transfer_mb="$(to_mbsec "$transfer_raw")"
        http_code=$(get_http_code "$url")

        printf '%s,%s,%s,%s,%s,%s,%s,%s,"%s","%s",%s,OK\n' \
            "$conn" "$req_sec" "$latency_avg" "$latency_stdev" "$latency_max" \
            "$transfer_mb" "$total_requests" "$non_2xx" "$name" "$url" "$http_code" \
            >> "$CSV_FILE"

        printf 'OK (%.1f req/s)\n' "$req_sec"
        sleep 2
    done

    i=$((i + 1))
done

# --- GRAFICI (STILE ORIGINALE) ---
cat > "$GNUPLOT_SCRIPT" <<GP
set datafile separator ','
set key outside right top
set grid
set border lw 1.2
set style line 1 lc rgb '#2563eb' lw 2 pt 7 ps 1.1
set style line 2 lc rgb '#dc2626' lw 2 pt 7 ps 1.1
set style line 3 lc rgb '#d97706' lw 2 pt 7 ps 1.1
set style line 4 lc rgb '#16a34a' lw 2 pt 7 ps 1.1
set style line 5 lc rgb '#7c3aed' lw 2 pt 7 ps 1.1

set terminal svg size 1200,480 dynamic enhanced font 'Arial,11'

set output '${ASSETS_DIR}/throughput.svg'
set title 'Throughput by Concurrent Connections'
set xlabel 'Concurrent connections'
set ylabel 'Requests/sec'
plot '${CSV_FILE}' using 1:2 skip 1 with linespoints ls 1 title 'Requests/sec'

set output '${ASSETS_DIR}/latency.svg'
set title 'Average and Max Latency'
set xlabel 'Concurrent connections'
set ylabel 'Latency (ms)'
plot '${CSV_FILE}' using 1:3 skip 1 with linespoints ls 2 title 'Average latency', \
     '${CSV_FILE}' using 1:5 skip 1 with linespoints ls 3 title 'Max latency'

set output '${ASSETS_DIR}/latency_stdev.svg'
set title 'Latency Stability (Standard Deviation)'
set xlabel 'Concurrent connections'
set ylabel 'Std Dev (ms)'
plot '${CSV_FILE}' using 1:4 skip 1 with linespoints ls 5 title 'Latency stdev'

set output '${ASSETS_DIR}/transfer.svg'
set title 'Transfer Rate'
set xlabel 'Concurrent connections'
set ylabel 'MB/sec'
plot '${CSV_FILE}' using 1:6 skip 1 with linespoints ls 4 title 'Transfer MB/sec'

set output '${ASSETS_DIR}/efficiency.svg'
set title 'Connection Efficiency'
set xlabel 'Concurrent connections'
set ylabel 'Req/sec per connection'
plot '${CSV_FILE}' using 1:(\$2/\$1) skip 1 with linespoints ls 1 title 'Efficiency'
GP

gnuplot "$GNUPLOT_SCRIPT"

# --- STATISTICHE ---
peak_req=$(awk -F, 'NR>1 && $2>0 {print $2}' "$CSV_FILE" | sort -n | tail -1)
best_latency=$(awk -F, 'NR>1 && $3>0 {print $3}' "$CSV_FILE" | sort -n | head -1)
worst_latency=$(awk -F, 'NR>1 && $5>0 {print $5}' "$CSV_FILE" | sort -n -r | head -1)

# --- HTML REPORT ---
TABLE_ROWS="$(awk -F, 'NR>1 {
    status_class = "http-ok"
    if ($12 == "BINARY") status_class = "binary"
    if ($12 == "HEALTH_FAILED") status_class = "http-error"
    printf "            <tr><td>%s</td><td>%s</td><td>%s</td><td>%.1f</td><td>%.2f</td><td>%.2f</td><td>%.2f</td><td>%s</td><td class=\"%s\">%s</td><td>%s</td></tr>\n",
           $9, $10, $1, $2, $3, $5, $6, $7, status_class, $11, $12
}' "$CSV_FILE")"

cat > "$HTML_FILE" <<HTML
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>MiniWeb Benchmark Report</title>
  <link rel="stylesheet" href="/static/css/custom.css" />
  <style>
    .benchmark-grid { display:grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr)); gap: 1rem; margin: 1rem 0 1.5rem; }
    .stat-card h3 { margin: 0 0 .35rem; font-size: .95rem; color: var(--text-muted); }
    .stat-card p { margin: 0; font-size: 1.3rem; font-weight: 700; }
    .graphs { display:grid; gap: 1rem; }
    .graph img { width:100%; height:auto; border:1px solid var(--border); border-radius: .8rem; background-color: #ccc; }
    .details { overflow:auto; }
    table { width:100%; border-collapse: collapse; }
    th, td { text-align:left; padding: .55rem .6rem; border-bottom: 1px solid var(--border); font-size: .95rem; }
    th { color: var(--text-muted); font-weight: 600; }
    .meta { font-size: .95rem; color: var(--text-muted); }
    code { background: var(--surface-2); padding: 0.1rem 0.4rem; border-radius: .4rem; }
    .http-ok { color: #10b981; }
    .http-error { color: #ef4444; }
    .binary { color: #8b5cf6; font-style: italic; }
  </style>
</head>
<body>
  <header class="site-header">
    <div class="container header-row">
      <a class="brand" href="/">MiniWeb</a>
      <nav class="navbar-menu"><a class="nav-link active" href="/static/benchmark.html">Benchmark</a></nav>
      <button class="theme-toggle" id="themeToggle" type="button" aria-label="Toggle theme">◐</button>
    </div>
  </header>

  <main class="app-shell">
    <div class="container">
      <section class="panel">
        <h1>Benchmark Report</h1>
        <p class="meta">Generated: ${TIMESTAMP} · Base URL: ${BASE_URL} · Duration: ${TEST_DURATION}s · Threads: ${THREADS}</p>

        <div class="benchmark-grid">
          <article class="panel stat-card"><h3>Peak throughput</h3><p>${peak_req:-0} req/s</p></article>
          <article class="panel stat-card"><h3>Best avg latency</h3><p>${best_latency:-0} ms</p></article>
          <article class="panel stat-card"><h3>Worst max latency</h3><p>${worst_latency:-0} ms</p></article>
        </div>

        <div class="graphs">
          <article class="graph"><h2>Throughput</h2><img src="/static/benchmark_assets/throughput.svg" alt="Throughput graph"></article>
          <article class="graph"><h2>Latency (avg vs max)</h2><img src="/static/benchmark_assets/latency.svg" alt="Latency graph"></article>
          <article class="graph"><h2>Latency standard deviation</h2><img src="/static/benchmark_assets/latency_stdev.svg" alt="Latency deviation graph"></article>
          <article class="graph"><h2>Transfer rate</h2><img src="/static/benchmark_assets/transfer.svg" alt="Transfer graph"></article>
          <article class="graph"><h2>Connection efficiency</h2><img src="/static/benchmark_assets/efficiency.svg" alt="Efficiency graph"></article>
        </div>
      </section>

      <section class="panel details" style="margin-top:1rem;">
        <h2>Detailed run stats</h2>
        <table>
          <thead>
            <tr>
              <th>Endpoint</th>
              <th>URL</th>
              <th>Connections</th>
              <th>Req/sec</th>
              <th>Avg latency (ms)</th>
              <th>Max latency (ms)</th>
              <th>Transfer (MB/s)</th>
              <th>Total requests</th>
              <th>HTTP</th>
              <th>Status</th>
            </tr>
          </thead>
          <tbody>
${TABLE_ROWS}
          </tbody>
        </table>
      </section>
    </div>
  </main>
  <script src="/static/js/theme_toggler.js"></script>
</body>
</html>
HTML

printf '\nBenchmark complete.\n'
printf '  HTML report : http://localhost:%s/static/benchmark.html\n' "$SERVER_PORT"
printf '  CSV results : %s\n' "$CSV_FILE"
printf '  Assets dir  : %s\n' "$ASSETS_DIR"
