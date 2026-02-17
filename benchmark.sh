#!/bin/ksh
#
# benchmark.sh - Build a detailed benchmark report for MiniWeb.
# Generates static/benchmark.html and per-metric SVG graphs in static/benchmark_assets.
#

set -eu
SERVER_PORT=9001
SERVER_URL="${SERVER_URL:-http://localhost:9001/apiroot}"
TEST_DURATION="${TEST_DURATION:-30}"
THREADS="${THREADS:-4}"
CONNECTIONS="${CONNECTIONS:-4 8 16 32 64 128}"
OUTPUT_ROOT="${OUTPUT_ROOT:-static}"
ASSETS_DIR="${OUTPUT_ROOT}/benchmark_assets"
TIMESTAMP="$(date '+%Y-%m-%d %H:%M:%S')"
CSV_FILE="${ASSETS_DIR}/results.csv"
HTML_FILE="${OUTPUT_ROOT}/benchmark.html"
GNUPLOT_SCRIPT="${ASSETS_DIR}/plot.gp"

printf 'MiniWeb Benchmark\n'
printf '=============================================\n'
printf 'Server URL  : %s\n' "$SERVER_URL"
printf 'Threads     : %s\n' "$THREADS"
printf 'Duration    : %ss\n' "$TEST_DURATION"
printf 'Connections : %s\n\n' "$CONNECTIONS"

for cmd in wrk gnuplot curl awk sed; do
  if ! command -v "$cmd" >/dev/null 2>&1; then
    echo "ERROR: missing dependency '$cmd'" >&2
    exit 1
  fi
done

if ! curl -s --max-time 3 "$SERVER_URL" >/dev/null 2>&1; then
  echo "ERROR: server is unreachable at $SERVER_URL" >&2
  exit 1
fi

mkdir -p "$ASSETS_DIR"

cat > "$CSV_FILE" <<'CSV'
connections,req_sec,latency_avg_ms,latency_stdev_ms,latency_max_ms,transfer_mb_sec,total_requests,errors_non_2xx
CSV

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

for conn in $CONNECTIONS; do
  echo "Running test with $conn connections..."
  run_output="$(wrk -t"$THREADS" -c"$conn" -d"${TEST_DURATION}"s --latency "$SERVER_URL" 2>&1)"
  echo "$run_output" > "${ASSETS_DIR}/wrk_${conn}.txt"

  req_sec="$(echo "$run_output" | awk '/Requests\/sec:/ {print $2; exit}')"
  transfer_raw="$(echo "$run_output" | awk '/Transfer\/sec:/ {print $2; exit}')"
  total_requests="$(echo "$run_output" | awk '/requests in/ {print $1; exit}')"
  non_2xx="$(echo "$run_output" | awk '/Non-2xx or 3xx responses:/ {print $5; exit}')"
  latency_line="$(echo "$run_output" | awk '/Latency/ {print; exit}')"

  [ -z "$req_sec" ] && req_sec="0"
  [ -z "$transfer_raw" ] && transfer_raw="0MB"
  [ -z "$total_requests" ] && total_requests="0"
  [ -z "$non_2xx" ] && non_2xx="0"

  latency_avg="$(to_ms "$(echo "$latency_line" | awk '{print $2}')")"
  latency_stdev="$(to_ms "$(echo "$latency_line" | awk '{print $3}')")"
  latency_max="$(to_ms "$(echo "$latency_line" | awk '{print $4}')")"
  transfer_mb="$(to_mbsec "$transfer_raw")"

  printf '%s,%s,%s,%s,%s,%s,%s,%s\n' \
    "$conn" "$req_sec" "$latency_avg" "$latency_stdev" "$latency_max" "$transfer_mb" "$total_requests" "$non_2xx" \
    >> "$CSV_FILE"
  sleep 5
done

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

peak_req=$(tail -n +2 "$CSV_FILE" | cut -d',' -f2 | sort -n | tail -1)
best_latency=$(tail -n +2 "$CSV_FILE" | cut -d',' -f3 | sort -n | head -1)
worst_latency=$(tail -n +2 "$CSV_FILE" | cut -d',' -f5 | sort -n -r | head -1)

peak_req_value="${peak_req}"
peak_req_conn="${peak_req##*|}"
best_lat_value="${best_latency}"
best_lat_conn="${best_latency##*|}"
worst_lat_value="${worst_latency}"
worst_lat_conn="${worst_latency##*|}"

TABLE_ROWS="$(awk -F',' 'NR>1{printf "            <tr><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td><td>%s</td></tr>\n",$1,$2,$3,$4,$5,$6,$7,$8}' "$CSV_FILE")"

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
        <p class="meta">Generated: ${TIMESTAMP} · Target URL: <code>${SERVER_URL}</code> · Duration: ${TEST_DURATION}s · Threads: ${THREADS}</p>

        <div class="benchmark-grid">
          <article class="panel stat-card"><h3>Peak throughput</h3><p>${peak_req_value} req/s</p><span class="muted">at ${peak_req_conn} connections</span></article>
          <article class="panel stat-card"><h3>Best avg latency</h3><p>${best_lat_value} ms</p><span class="muted">at ${best_lat_conn} connections</span></article>
          <article class="panel stat-card"><h3>Worst max latency</h3><p>${worst_lat_value} ms</p><span class="muted">at ${worst_lat_conn} connections</span></article>
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
              <th>Connections</th>
              <th>Req/sec</th>
              <th>Avg latency (ms)</th>
              <th>StdDev (ms)</th>
              <th>Max latency (ms)</th>
              <th>Transfer (MB/s)</th>
              <th>Total requests</th>
              <th>Non-2xx/3xx</th>
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
printf '  HTML report : %s%s%s\n' "http://localhost:" "$SERVER_PORT/" "$HTML_FILE"
printf '  CSV results : %s\n' "$CSV_FILE"
printf '  Assets dir  : %s\n' "$ASSETS_DIR"
