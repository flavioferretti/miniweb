#!/bin/ksh
#
# advanced_bench.ksh - Advanced benchmark with detailed graphs
#

set -e

SERVER_URL="http://localhost:9001/"
TEST_DURATION=20
THREADS=4
set -A CONNECTIONS 10 25 50 100 150 200 250 300 350
OUTPUT_DIR="benchmark"
TIMESTAMP=$(date +%Y%m%d_%H%M%S)

echo "Advanced Benchmark Suite ${TIMESTAMP}"
echo "====================================================="
echo " - URL ${SERVER_URL}"

# Check dependencies
for cmd in wrk gnuplot curl; do
    if ! command -v $cmd >/dev/null 2>&1; then
        echo "ERROR: $cmd not found. Install with: pkg_add $cmd"
        exit 1
    fi
done

# Check server
if ! curl -s --max-time 2 "${SERVER_URL}" >/dev/null 2>&1; then
    echo "ERROR: Server not running"
    exit 1
fi

echo "Server OK"
echo ""

# Create output directory
mkdir -p "${OUTPUT_DIR}"

# CSV file
CSV="${OUTPUT_DIR}/results_${TIMESTAMP}.csv"
echo "connections,req_sec,latency_avg_ms,latency_max_ms,latency_stdev_ms,transfer_mb,total_requests" > "$CSV"

# Run benchmarks
for conn in "${CONNECTIONS[@]}"; do
    echo "Testing: $conn connections..."

    output=$(wrk -t${THREADS} -c${conn} -d${TEST_DURATION}s --latency "${SERVER_URL}" 2>&1)

    # Save full output
    echo "$output" > "${OUTPUT_DIR}/wrk_${conn}_${TIMESTAMP}.txt"

    # Parse metrics
    req_sec=$(echo "$output" | grep "Requests/sec:" | awk '{print $2}')

    # Latency metrics
    latency_line=$(echo "$output" | grep "Latency" | head -1)
    latency_avg=$(echo "$latency_line" | awk '{print $2}' | sed 's/ms//')
    latency_stdev=$(echo "$latency_line" | awk '{print $3}' | sed 's/ms//')
    latency_max=$(echo "$latency_line" | awk '{print $4}' | sed 's/ms//')

    # Convert if in microseconds or seconds
    for lat in latency_avg latency_stdev latency_max; do
        val=$(eval echo \$$lat)
        if echo "$val" | grep -q "us"; then
            val=$(echo "$val" | sed 's/us//' | awk '{printf "%.2f", $1/1000}')
        elif echo "$val" | grep -q "s"; then
            val=$(echo "$val" | sed 's/s//' | awk '{printf "%.2f", $1*1000}')
        fi
        eval $lat="$val"
    done

    transfer=$(echo "$output" | grep "Transfer/sec:" | awk '{print $2}' | sed 's/MB//')
    total=$(echo "$output" | grep "requests in" | awk '{print $1}')

    echo "$conn,$req_sec,$latency_avg,$latency_max,$latency_stdev,$transfer,$total" >> "$CSV"

    echo "  -> $req_sec req/s, ${latency_avg}ms latency"
    sleep 6 # lowering cpu temp
done

echo ""
echo "Generating graphs..."

# Create advanced gnuplot script
PLOT_SCRIPT="${OUTPUT_DIR}/plot_${TIMESTAMP}.gnuplot"

cat > "$PLOT_SCRIPT" << 'GNUPLOT_EOF'
#!/usr/local/bin/gnuplot

set terminal svg size 1600,1200 enhanced font 'Arial,11' background rgb 'white'
set output 'OUTPUT_FILE'

set datafile separator ','
set grid
set key outside right top

# Color scheme
throughput_color = "#0066CC"
latency_color = "#CC0000"
stdev_color = "#FF9900"
transfer_color = "#00AA00"

set multiplot layout 3,2 title "MiniWeb Server Performance Analysis" font 'Arial,18'

# === Plot 1: Throughput and Latency (dual axis) ===
set title 'Throughput vs Latency' font 'Arial,14'
set xlabel 'Concurrent Connections'
set ylabel 'Requests/sec' textcolor rgb throughput_color
set y2label 'Latency (ms)' textcolor rgb latency_color
set ytics nomirror textcolor rgb throughput_color
set y2tics textcolor rgb latency_color
set style fill solid 0.3
plot 'CSV_FILE' using 1:2 skip 1 with linespoints lw 3 pt 7 ps 1.5 lc rgb throughput_color title 'Throughput' axes x1y1, \
     'CSV_FILE' using 1:3 skip 1 with linespoints lw 3 pt 9 ps 1.5 lc rgb latency_color title 'Avg Latency' axes x1y2

# === Plot 2: Throughput only ===
unset y2label
unset y2tics
set ytics mirror textcolor rgb "black"
set title 'Server Throughput' font 'Arial,14'
set ylabel 'Requests/sec'
set style fill solid 0.5
plot 'CSV_FILE' using 1:2 skip 1 with filledcurves x1 lc rgb throughput_color notitle, \
     'CSV_FILE' using 1:2 skip 1 with linespoints lw 3 pt 7 ps 1.5 lc rgb throughput_color title 'Req/sec'

# === Plot 3: Latency breakdown ===
set title 'Latency Analysis' font 'Arial,14'
set ylabel 'Latency (ms)'
plot 'CSV_FILE' using 1:3 skip 1 with linespoints lw 2 pt 7 lc rgb latency_color title 'Average', \
     'CSV_FILE' using 1:5 skip 1 with linespoints lw 2 pt 11 lc rgb stdev_color title 'Std Dev'
     #'CSV_FILE' using 1:4 skip 1 with linespoints lw 2 pt 9 lc rgb "#FF0000" title 'Maximum', \


# === Plot 4: Latency with error bars ===
set title 'Latency with Standard Deviation' font 'Arial,14'
set ylabel 'Latency (ms)'
plot 'CSV_FILE' using 1:3:5 skip 1 with yerrorbars lw 2 pt 7 ps 1.5 lc rgb latency_color title 'Avg ± StdDev', \
     'CSV_FILE' using 1:3 skip 1 with lines lw 2 lc rgb latency_color notitle

# === Plot 5: Transfer rate ===
set title 'Data Transfer Rate' font 'Arial,14'
set ylabel 'Transfer (MB/sec)'
set style fill solid 0.5
plot 'CSV_FILE' using 1:6 skip 1 with filledcurves x1 lc rgb transfer_color notitle, \
     'CSV_FILE' using 1:6 skip 1 with linespoints lw 3 pt 7 ps 1.5 lc rgb transfer_color title 'MB/sec'

# === Plot 6: Efficiency (req/sec per connection) ===
set title 'Connection Efficiency' font 'Arial,14'
set ylabel 'Requests per Connection'
plot 'CSV_FILE' using 1:($2/$1) skip 1 with linespoints lw 3 pt 7 ps 1.5 lc rgb "#9900CC" title 'Req/sec per Connection'

unset multiplot
GNUPLOT_EOF

# Replace placeholders
OUTPUT_SVG="${OUTPUT_DIR}/benchmark_${TIMESTAMP}.svg"
sed "s|OUTPUT_FILE|${OUTPUT_SVG}|g" "$PLOT_SCRIPT" | sed "s|CSV_FILE|${CSV}|g" > "${PLOT_SCRIPT}.tmp"
mv "${PLOT_SCRIPT}.tmp" "$PLOT_SCRIPT"

# Generate graph
gnuplot "$PLOT_SCRIPT"

echo ""
echo "====================================="
echo "Benchmark Complete!"
echo "====================================="
echo ""
echo "Results:"
echo "  CSV:   $CSV"
echo "  Graph: $OUTPUT_SVG"
echo ""
echo "Summary:"
tail -n +2 "$CSV" | awk -F',' '{printf "  %3d conn: %8.2f req/s, %6.2f ms latency\n", $1, $2, $3}'
echo ""

# Find peak performance
peak_req=$(tail -n +2 "$CSV" | cut -d',' -f2 | sort -n | tail -1)
best_latency=$(tail -n +2 "$CSV" | cut -d',' -f3 | sort -n | head -1)

echo "Peak throughput: $peak_req req/s"
echo "Best latency:    $best_latency ms"
echo ""
