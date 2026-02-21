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

# ---------------------------------------------------------------------------
# Endpoint definitions  (IDs, URLs, human names — must stay in sync)
# PDF and PS endpoints are excluded: wrk cannot benchmark binary streams.
# ---------------------------------------------------------------------------
set -A ENDPOINT_IDS -- \
    "STATIC_HTML"    "STATIC_CSS"      "STATIC_JS"       "STATIC_IMG" \
    "PAGE_HOME"      "PAGE_DOCS"       "PAGE_NETWORKING" "PACKAGES_UI"    "API_ROOT" \
    "API_METRICS"    "API_NETWORKING" \
    "API_PKG_SEARCH" "API_PKG_INFO"    "API_PKG_FILES" \
    "MAN_SEARCH"     "MAN_HTML"        "MAN_HTML_EXT"    "MAN_MD"

set -A ENDPOINT_URLS -- \
    "/static/test.html"                    "/static/css/custom.css"            "/static/js/theme_toggler.js"  "/static/assets/favicon.svg" \
    "/"                                    "/docs"                             "/networking"                  "/packages"                  "/apiroot" \
    "/api/metrics"                         "/api/networking" \
    "/api/packages/search?q=curl"          "/api/packages/info?name=curl"      "/api/packages/files?name=curl" \
    "/api/man/search?q=pledge"             "/man/1/1/ls"                       "/man/1/1/ls.html"             "/man/1/1/ls.md"

set -A ENDPOINT_NAMES -- \
    "Static HTML"         "Static CSS"           "Static JS"             "Static Image" \
    "Home Dashboard"      "Documentation"        "Networking"            "Packages UI"          "API Index" \
    "System Metrics API"  "Network Info API" \
    "Package Search"      "Package Info"         "Package Files" \
    "Man Search"          "Man Page (HTML)"      "Man Page (.html)"      "Man Page (Markdown)"

# ---------------------------------------------------------------------------
# Helper functions
# ---------------------------------------------------------------------------
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
        us)    awk -v n="$num" 'BEGIN { printf "%.3f", n/1000 }' ;;
        ms|'') awk -v n="$num" 'BEGIN { printf "%.3f", n }' ;;
        s)     awk -v n="$num" 'BEGIN { printf "%.3f", n*1000 }' ;;
        m)     awk -v n="$num" 'BEGIN { printf "%.3f", n*60000 }' ;;
        *)     awk -v n="$num" 'BEGIN { printf "%.3f", n }' ;;
    esac
}

to_mbsec() {
    typeset raw num unit
    raw="$(trim_value "$1" | tr -d ',')"
    num="$(printf '%s' "$raw" | sed 's/[^0-9.]//g')"
    unit="$(printf '%s' "$raw" | sed 's/[0-9.]//g')"
    [ -z "$num" ] && num="0"
    case "$unit" in
        B)     awk -v n="$num" 'BEGIN { printf "%.3f", n/1048576 }' ;;
        KB)    awk -v n="$num" 'BEGIN { printf "%.3f", n/1024 }' ;;
        MB|'') awk -v n="$num" 'BEGIN { printf "%.3f", n }' ;;
        GB)    awk -v n="$num" 'BEGIN { printf "%.3f", n*1024 }' ;;
        *)     awk -v n="$num" 'BEGIN { printf "%.3f", n }' ;;
    esac
}

check_endpoint() {
    typeset url="$1"
    curl -s -G --max-time 10 -o /dev/null -w '%{http_code}' "${BASE_URL}${url}" 2>/dev/null || printf '000'
}

get_http_code() {
    typeset url="$1"
    curl -s -G --max-time 3 -o /dev/null -w '%{http_code}' "${BASE_URL}${url}" 2>/dev/null || printf '000'
}

# ---------------------------------------------------------------------------
# Startup banner + dependency checks
# ---------------------------------------------------------------------------
printf 'MiniWeb Benchmark\n'
printf '=============================================\n'
printf 'Server URL  : %s\n' "$BASE_URL"
printf 'Threads     : %s\n' "$THREADS"
printf 'Duration    : %ss\n' "$TEST_DURATION"
printf 'Connections : %s\n\n' "$CONNECTIONS"

for cmd in wrk gnuplot curl awk sed; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        printf 'ERROR: missing dependency "%s"\n' "$cmd" >&2
        exit 1
    fi
done

if ! curl -s --max-time 3 "${BASE_URL}/static/test.html" >/dev/null 2>&1; then
    printf 'ERROR: server is unreachable at %s\n' "$BASE_URL" >&2
    exit 1
fi

mkdir -p "$ASSETS_DIR"

# CSV header
printf 'endpoint_id,connections,req_sec,latency_avg_ms,latency_stdev_ms,latency_max_ms,transfer_mb_sec,total_requests,errors_non_2xx,endpoint_name,endpoint_url,http_code,status\n' \
    > "$CSV_FILE"

# ---------------------------------------------------------------------------
# Run benchmarks
# ---------------------------------------------------------------------------
total_endpoints=${#ENDPOINT_IDS[*]}
i=0
while [ "$i" -lt "$total_endpoints" ]; do
    id="${ENDPOINT_IDS[$i]}"
    name="${ENDPOINT_NAMES[$i]}"
    url="${ENDPOINT_URLS[$i]}"
    safe_name="$(printf '%s' "$id" | tr '[:upper:]' '[:lower:]')"

    printf '\n▶ Testing: %s\n' "$name"
    printf '  URL: %s\n' "${BASE_URL}${url}"
    printf '  Health check ... '

    http_check="$(check_endpoint "$url")"

    case "$http_check" in
        200|201|202|204|301|302|304)
            printf 'OK (HTTP %s)\n' "$http_check"
            ;;
        *)
            printf 'FAILED (HTTP %s) — skipping endpoint\n' "$http_check"
            for conn in $CONNECTIONS; do
                printf '%s,%s,0,0,0,0,0,0,0,"%s","%s",%s,HEALTH_FAILED\n' \
                    "$id" "$conn" "$name" "$url" "$http_check" >> "$CSV_FILE"
            done
            i=$((i + 1))
            continue
            ;;
    esac

    for conn in $CONNECTIONS; do
        printf '  Connections: %3d ... ' "$conn"

        full_url="${BASE_URL}${url}"
        run_output="$(wrk -t"$THREADS" -c"$conn" -d"${TEST_DURATION}"s --latency "$full_url" 2>&1)"
        printf '%s\n' "$run_output" > "${ASSETS_DIR}/${safe_name}_${conn}.txt"

        req_sec="$(printf '%s\n' "$run_output" | awk '/Requests\/sec:/ {print $2; exit}')"
        [ -z "$req_sec" ] && req_sec="0"

        transfer_raw="$(printf '%s\n' "$run_output" | awk '/Transfer\/sec:/ {print $2; exit}')"
        [ -z "$transfer_raw" ] && transfer_raw="0MB"

        total_requests="$(printf '%s\n' "$run_output" | awk '/requests in/ {print $1; exit}')"
        [ -z "$total_requests" ] && total_requests="0"

        non_2xx="$(printf '%s\n' "$run_output" | awk '/Non-2xx or 3xx responses:/ {print $5; exit}')"
        [ -z "$non_2xx" ] && non_2xx="0"

        latency_line="$(printf '%s\n' "$run_output" | awk '/^[[:space:]]*Latency/ {print; exit}')"
        if [ -n "$latency_line" ]; then
            latency_avg="$(to_ms "$(printf '%s\n' "$latency_line" | awk '{print $2}')")"
            latency_stdev="$(to_ms "$(printf '%s\n' "$latency_line" | awk '{print $3}')")"
            latency_max="$(to_ms "$(printf '%s\n' "$latency_line" | awk '{print $4}')")"
        else
            latency_avg="0"; latency_stdev="0"; latency_max="0"
        fi

        transfer_mb="$(to_mbsec "$transfer_raw")"
        http_code="$(get_http_code "$url")"

        printf '%s,%s,%s,%s,%s,%s,%s,%s,%s,"%s","%s",%s,OK\n' \
            "$id" "$conn" "$req_sec" "$latency_avg" "$latency_stdev" "$latency_max" \
            "$transfer_mb" "$total_requests" "$non_2xx" "$name" "$url" "$http_code" \
            >> "$CSV_FILE"

        printf 'OK (%.1f req/s)\n' "$req_sec"
        sleep 2
    done

    i=$((i + 1))
done

# ---------------------------------------------------------------------------
# Gnuplot — overall graphs (aggregate all endpoints)
# ---------------------------------------------------------------------------
GNUPLOT_SCRIPT="${ASSETS_DIR}/plot_overall.gp"

# Build the overall gnuplot script using printf to avoid heredoc expansion issues
{
printf 'set datafile separator ","\n'
printf 'set key outside right top\n'
printf 'set grid\n'
printf 'set border lw 1.2\n'
printf 'set style line 1 lc rgb "#2563eb" lw 2 pt 7 ps 1.1\n'
printf 'set style line 2 lc rgb "#dc2626" lw 2 pt 7 ps 1.1\n'
printf 'set style line 3 lc rgb "#d97706" lw 2 pt 7 ps 1.1\n'
printf 'set style line 4 lc rgb "#16a34a" lw 2 pt 7 ps 1.1\n'
printf 'set style line 5 lc rgb "#7c3aed" lw 2 pt 7 ps 1.1\n'
printf 'set terminal svg size 1200,480 dynamic enhanced font "Arial,11"\n'
printf '\n'
printf 'set output "%s/throughput.svg"\n' "$ASSETS_DIR"
printf 'set title "Throughput by Concurrent Connections (all endpoints)"\n'
printf 'set xlabel "Concurrent connections"\n'
printf 'set ylabel "Requests/sec"\n'
printf 'plot "%s" using 2:3 skip 1 with linespoints ls 1 title "Requests/sec"\n' "$CSV_FILE"
printf '\n'
printf 'set output "%s/latency.svg"\n' "$ASSETS_DIR"
printf 'set title "Average and Max Latency (all endpoints)"\n'
printf 'set xlabel "Concurrent connections"\n'
printf 'set ylabel "Latency (ms)"\n'
printf 'plot "%s" using 2:4 skip 1 with linespoints ls 2 title "Avg latency", \\\n' "$CSV_FILE"
printf '     "%s" using 2:6 skip 1 with linespoints ls 3 title "Max latency"\n' "$CSV_FILE"
printf '\n'
printf 'set output "%s/latency_stdev.svg"\n' "$ASSETS_DIR"
printf 'set title "Latency Stability (all endpoints)"\n'
printf 'set xlabel "Concurrent connections"\n'
printf 'set ylabel "Std Dev (ms)"\n'
printf 'plot "%s" using 2:5 skip 1 with linespoints ls 5 title "Latency stdev"\n' "$CSV_FILE"
printf '\n'
printf 'set output "%s/transfer.svg"\n' "$ASSETS_DIR"
printf 'set title "Transfer Rate (all endpoints)"\n'
printf 'set xlabel "Concurrent connections"\n'
printf 'set ylabel "MB/sec"\n'
printf 'plot "%s" using 2:7 skip 1 with linespoints ls 4 title "Transfer MB/sec"\n' "$CSV_FILE"
printf '\n'
printf 'set output "%s/efficiency.svg"\n' "$ASSETS_DIR"
printf 'set title "Connection Efficiency (all endpoints)"\n'
printf 'set xlabel "Concurrent connections"\n'
printf 'set ylabel "Req/sec per connection"\n'
printf 'plot "%s" using 2:($3/$2) skip 1 with linespoints ls 1 title "Efficiency"\n' "$CSV_FILE"
} > "$GNUPLOT_SCRIPT"

gnuplot "$GNUPLOT_SCRIPT"

# ---------------------------------------------------------------------------
# Per-endpoint graphs
# ---------------------------------------------------------------------------
i=0
while [ "$i" -lt "$total_endpoints" ]; do
    id="${ENDPOINT_IDS[$i]}"
    name="${ENDPOINT_NAMES[$i]}"
    safe_name="$(printf '%s' "$id" | tr '[:upper:]' '[:lower:]')"
    ep_csv="${ASSETS_DIR}/${safe_name}.csv"
    gp_file="${ASSETS_DIR}/plot_${safe_name}.gp"

    # Extract per-endpoint CSV
    printf 'connections,req_sec,latency_avg_ms,latency_max_ms,transfer_mb_sec\n' > "$ep_csv"
    awk -F, -v eid="$id" 'NR>1 && $1==eid && $13=="OK" {printf "%s,%s,%s,%s,%s\n",$2,$3,$4,$6,$7}' \
        "$CSV_FILE" >> "$ep_csv"

    # Check if there is any data to plot
    lines="$(awk 'NR>1' "$ep_csv" | wc -l | tr -d ' ')"
    if [ "$lines" -gt 0 ]; then
        {
        printf 'set datafile separator ","\n'
        printf 'set key outside right top\n'
        printf 'set grid\n'
        printf 'set border lw 1.2\n'
        printf 'set style line 1 lc rgb "#2563eb" lw 2 pt 7 ps 1.1\n'
        printf 'set style line 2 lc rgb "#dc2626" lw 2 pt 7 ps 1.1\n'
        printf 'set style line 3 lc rgb "#16a34a" lw 2 pt 7 ps 1.1\n'
        printf 'set terminal svg size 900,380 dynamic enhanced font "Arial,11"\n'
        printf '\n'
        printf 'set output "%s/%s_throughput.svg"\n' "$ASSETS_DIR" "$safe_name"
        printf 'set title "%s — Throughput"\n' "$name"
        printf 'set xlabel "Concurrent connections"\n'
        printf 'set ylabel "Requests/sec"\n'
        printf 'plot "%s" using 1:2 skip 1 with linespoints ls 1 title "Req/sec"\n' "$ep_csv"
        printf '\n'
        printf 'set output "%s/%s_latency.svg"\n' "$ASSETS_DIR" "$safe_name"
        printf 'set title "%s — Latency"\n' "$name"
        printf 'set xlabel "Concurrent connections"\n'
        printf 'set ylabel "Latency (ms)"\n'
        printf 'plot "%s" using 1:3 skip 1 with linespoints ls 2 title "Avg latency", \\\n' "$ep_csv"
        printf '     "%s" using 1:4 skip 1 with linespoints ls 1 title "Max latency"\n' "$ep_csv"
        } > "$gp_file"
        gnuplot "$gp_file"
    fi

    i=$((i + 1))
done

# ---------------------------------------------------------------------------
# Summary statistics
# ---------------------------------------------------------------------------
peak_req="$(awk -F, 'NR>1 && $3+0>0 {print $3}' "$CSV_FILE" | sort -n | tail -1)"
best_latency="$(awk -F, 'NR>1 && $4+0>0 {print $4}' "$CSV_FILE" | sort -n | head -1)"
worst_latency="$(awk -F, 'NR>1 && $6+0>0 {print $6}' "$CSV_FILE" | sort -n | tail -1)"
avg_req="$(awk -F, 'NR>1 && $3+0>0 {s+=$3; c++} END {if(c>0) printf "%.1f", s/c; else print "0"}' "$CSV_FILE")"
avg_lat="$(awk -F, 'NR>1 && $4+0>0 {s+=$4; c++} END {if(c>0) printf "%.2f", s/c; else print "0"}' "$CSV_FILE")"

# ---------------------------------------------------------------------------
# Build per-endpoint HTML sections
# ---------------------------------------------------------------------------
ENDPOINT_SECTIONS=""
i=0
while [ "$i" -lt "$total_endpoints" ]; do
    id="${ENDPOINT_IDS[$i]}"
    name="${ENDPOINT_NAMES[$i]}"
    url="${ENDPOINT_URLS[$i]}"
    safe_name="$(printf '%s' "$id" | tr '[:upper:]' '[:lower:]')"

    # Table rows for this endpoint
    ep_rows="$(awk -F, -v eid="$id" 'NR>1 && $1==eid {
        sc="http-ok"
        if ($13=="HEALTH_FAILED") sc="http-error"
        printf "<tr><td>%s</td><td>%.1f</td><td>%.2f</td><td>%.2f</td><td>%.3f</td><td>%s</td><td class=\"%s\">%s</td><td>%s</td></tr>\n",
            $2, $3, $4, $6, $7, $8, sc, $12, $13
    }' "$CSV_FILE")"

    # Check if per-endpoint graphs exist
    tput_svg="${ASSETS_DIR}/${safe_name}_throughput.svg"
    lat_svg="${ASSETS_DIR}/${safe_name}_latency.svg"
    graph_block=""
    if [ -f "$tput_svg" ] && [ -f "$lat_svg" ]; then
        graph_block="<div class=\"ep-graphs\">
              <div class=\"graph\" data-src=\"/static/benchmark_assets/${safe_name}_throughput.svg\" data-title=\"${name} — Throughput\" title=\"Click to expand\">
                <img src=\"/static/benchmark_assets/${safe_name}_throughput.svg\" alt=\"Throughput ${name}\">
              </div>
              <div class=\"graph\" data-src=\"/static/benchmark_assets/${safe_name}_latency.svg\" data-title=\"${name} — Latency\" title=\"Click to expand\">
                <img src=\"/static/benchmark_assets/${safe_name}_latency.svg\" alt=\"Latency ${name}\">
              </div>
            </div>"
    fi

    section="
      <section class=\"panel ep-section\" id=\"ep-${safe_name}\">
        <h2>${name}</h2>
        <p class=\"meta ep-url\"><code>${url}</code></p>
        ${graph_block}
        <div class=\"details\">
          <table>
            <thead>
              <tr>
                <th>Connections</th><th>Req/sec</th><th>Avg Latency (ms)</th>
                <th>Max Latency (ms)</th><th>Transfer (MB/s)</th>
                <th>Total requests</th><th>HTTP</th><th>Status</th>
              </tr>
            </thead>
            <tbody>
${ep_rows}
            </tbody>
          </table>
        </div>
      </section>"

    ENDPOINT_SECTIONS="${ENDPOINT_SECTIONS}${section}"
    i=$((i + 1))
done

# ---------------------------------------------------------------------------
# Build endpoint nav links
# ---------------------------------------------------------------------------
NAV_LINKS=""
i=0
while [ "$i" -lt "$total_endpoints" ]; do
    id="${ENDPOINT_IDS[$i]}"
    name="${ENDPOINT_NAMES[$i]}"
    safe_name="$(printf '%s' "$id" | tr '[:upper:]' '[:lower:]')"
    NAV_LINKS="${NAV_LINKS}<a href=\"#ep-${safe_name}\" class=\"ep-nav-link\">${name}</a>"
    i=$((i + 1))
done

# ---------------------------------------------------------------------------
# Write HTML report
# ---------------------------------------------------------------------------
{
cat <<HTMLHEAD
<!doctype html>
<html lang="en">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>MiniWeb Benchmark Report</title>
  <link rel="stylesheet" href="/static/css/custom.css" />
  <style>
    /* ---- Benchmark-specific styles ---- */
    .benchmark-grid {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));
      gap: 1rem;
      margin: 1rem 0 1.5rem;
    }
    .stat-card h3 { margin: 0 0 .35rem; font-size: .9rem; color: var(--text-muted); text-transform: uppercase; letter-spacing: .04em; }
    .stat-card p  { margin: 0; font-size: 1.4rem; font-weight: 700; }
    .meta         { font-size: .9rem; color: var(--text-muted); }
    .ep-url       { margin-bottom: .75rem; }
    code          { background: var(--surface-2); padding: .1rem .4rem; border-radius: .35rem; font-size: .9em; }
    .graphs, .ep-graphs {
      display: grid;
      grid-template-columns: repeat(auto-fit, minmax(420px, 1fr));
      gap: 1rem;
      margin-bottom: 1rem;
    }
    .graph img    { width: 100%; height: auto; border: 1px solid var(--border); border-radius: .7rem; }
    .details      { overflow-x: auto; }
    table         { width: 100%; border-collapse: collapse; font-size: .9rem; }
    th, td        { text-align: left; padding: .45rem .6rem; border-bottom: 1px solid var(--border); }
    th            { color: var(--text-muted); font-weight: 600; white-space: nowrap; }
    .http-ok      { color: #10b981; font-weight: 600; }
    .http-error   { color: #ef4444; font-weight: 600; }
    /* Endpoint nav */
    .ep-nav       { display: flex; flex-wrap: wrap; gap: .4rem; margin: 1rem 0 1.5rem; }
    .ep-nav-link  {
      display: inline-block;
      padding: .25rem .65rem;
      border-radius: 99px;
      font-size: .82rem;
      background: var(--surface-2);
      color: var(--text);
      text-decoration: none;
      border: 1px solid var(--border);
      transition: background .15s;
    }
    .ep-nav-link:hover { background: var(--surface-3, #e2e8f0); }
    .ep-section   { margin-top: 1.5rem; scroll-margin-top: 4rem; }
    .overall-section { margin-top: .5rem; }
    /* ---- Graph card click-to-expand ---- */
    .graph        { cursor: pointer; position: relative; }
    .graph::after {
      content: '⤢';
      position: absolute; top: .45rem; right: .55rem;
      font-size: .8rem; opacity: .45;
      pointer-events: none;
    }
    .graph:hover::after { opacity: .85; }
    .graph:hover img { opacity: .92; }
    /* ---- Fullscreen modal ---- */
    #img-modal {
      display: none;
      position: fixed; inset: 0; z-index: 9999;
      background: rgba(0,0,0,.78);
      backdrop-filter: blur(4px);
      align-items: center; justify-content: center;
      padding: 1.5rem;
    }
    #img-modal.open { display: flex; }
    #img-modal .modal-inner {
      position: relative;
      max-width: min(92vw, 1300px);
      max-height: 90vh;
      background: var(--surface, #fff);
      border-radius: 1rem;
      box-shadow: 0 24px 80px rgba(0,0,0,.45);
      overflow: hidden;
      display: flex; flex-direction: column;
    }
    #img-modal .modal-header {
      display: flex; align-items: center; justify-content: space-between;
      padding: .7rem 1rem .55rem;
      border-bottom: 1px solid var(--border);
      flex-shrink: 0;
    }
    #img-modal .modal-title {
      font-size: .95rem; font-weight: 600;
      white-space: nowrap; overflow: hidden; text-overflow: ellipsis;
    }
    #img-modal .modal-close {
      background: none; border: none; cursor: pointer;
      font-size: 1.3rem; line-height: 1;
      color: var(--text-muted); padding: .1rem .3rem;
      border-radius: .35rem; flex-shrink: 0; margin-left: .75rem;
    }
    #img-modal .modal-close:hover { background: var(--surface-2); color: var(--text); }
    #img-modal .modal-body {
      overflow: auto; padding: 1rem;
      display: flex; align-items: center; justify-content: center;
    }
    #img-modal .modal-body img {
      max-width: 100%; max-height: calc(90vh - 60px);
      border-radius: .5rem; display: block;
    }
  </style>
</head>
<body>
  <header class="site-header">
    <div class="container header-row">
      <a class="brand" href="/">MiniWeb</a>
      <nav class="navbar-menu"><a class="nav-link active" href="/static/benchmark.html">Benchmark</a></nav>
      <button class="theme-toggle" id="themeToggle" type="button" aria-label="Toggle theme">&#9680;</button>
    </div>
  </header>

  <main class="app-shell">
    <div class="container">

      <!-- ===== SUMMARY ===== -->
      <section class="panel overall-section">
        <h1>Benchmark Report</h1>
        <p class="meta">Generated: ${TIMESTAMP} &nbsp;·&nbsp; Base URL: <code>${BASE_URL}</code> &nbsp;·&nbsp; Duration: ${TEST_DURATION}s &nbsp;·&nbsp; Threads: ${THREADS}</p>

        <div class="benchmark-grid">
          <article class="panel stat-card"><h3>Peak throughput</h3><p>${peak_req:-0} req/s</p></article>
          <article class="panel stat-card"><h3>Avg throughput</h3><p>${avg_req:-0} req/s</p></article>
          <article class="panel stat-card"><h3>Best avg latency</h3><p>${best_latency:-0} ms</p></article>
          <article class="panel stat-card"><h3>Worst max latency</h3><p>${worst_latency:-0} ms</p></article>
          <article class="panel stat-card"><h3>Grand avg latency</h3><p>${avg_lat:-0} ms</p></article>
        </div>

      </section>

      <!-- ===== ENDPOINT NAV ===== -->
      <section class="panel" style="margin-top:1rem;">
        <h2>Jump to endpoint</h2>
        <nav class="ep-nav">
HTMLHEAD

printf '%s\n' "$NAV_LINKS"

cat <<HTMLMID
        </nav>
      </section>

      <!-- ===== PER-ENDPOINT SECTIONS ===== -->
HTMLMID

printf '%s\n' "$ENDPOINT_SECTIONS"

cat <<HTMLFOOT
    </div>
  </main>
  <script src="/static/js/theme_toggler.js"></script>

  <!-- ===== IMAGE MODAL ===== -->
  <div id="img-modal" role="dialog" aria-modal="true" aria-label="Graph fullscreen view">
    <div class="modal-inner" id="modal-inner">
      <div class="modal-header">
        <span class="modal-title" id="modal-title"></span>
        <button class="modal-close" id="modal-close" aria-label="Close">&times;</button>
      </div>
      <div class="modal-body">
        <img id="modal-img" src="" alt="" />
      </div>
    </div>
  </div>

  <script>
    (function () {
      var modal    = document.getElementById('img-modal');
      var modalImg = document.getElementById('modal-img');
      var modalTtl = document.getElementById('modal-title');

      function openModal(src, title) {
        modalImg.src = src;
        modalImg.alt = title;
        modalTtl.textContent = title;
        modal.classList.add('open');
        document.body.style.overflow = 'hidden';
      }

      function closeModal() {
        modal.classList.remove('open');
        document.body.style.overflow = '';
        modalImg.src = '';
      }

      // Click on graph cards
      document.querySelectorAll('.graph[data-src]').forEach(function (card) {
        card.addEventListener('click', function () {
          openModal(card.dataset.src, card.dataset.title || '');
        });
      });

      // Close button
      document.getElementById('modal-close').addEventListener('click', closeModal);

      // Click outside modal inner
      modal.addEventListener('click', function (e) {
        if (e.target === modal) closeModal();
      });

      // Escape key
      document.addEventListener('keydown', function (e) {
        if (e.key === 'Escape' && modal.classList.contains('open')) closeModal();
      });
    })();
  </script>
</body>
</html>
HTMLFOOT
} > "$HTML_FILE"

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
printf '\nBenchmark complete.\n'
printf '▶  HTML report : http://localhost:%s/static/benchmark.html\n' "$SERVER_PORT"
printf '▶  CSV results : %s\n' "$CSV_FILE"
printf '▶  Assets dir  : %s\n' "$ASSETS_DIR"
