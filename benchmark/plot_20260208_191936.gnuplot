#!/usr/local/bin/gnuplot

set terminal svg size 1600,1200 enhanced font 'Arial,11' background rgb 'white'
set output 'benchmark/benchmark_20260208_191936.svg'

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
plot 'benchmark/results_20260208_191936.csv' using 1:2 skip 1 with linespoints lw 3 pt 7 ps 1.5 lc rgb throughput_color title 'Throughput' axes x1y1, \
     'benchmark/results_20260208_191936.csv' using 1:3 skip 1 with linespoints lw 3 pt 9 ps 1.5 lc rgb latency_color title 'Avg Latency' axes x1y2

# === Plot 2: Throughput only ===
unset y2label
unset y2tics
set ytics mirror textcolor rgb "black"
set title 'Server Throughput' font 'Arial,14'
set ylabel 'Requests/sec'
set style fill solid 0.5
plot 'benchmark/results_20260208_191936.csv' using 1:2 skip 1 with filledcurves x1 lc rgb throughput_color notitle, \
     'benchmark/results_20260208_191936.csv' using 1:2 skip 1 with linespoints lw 3 pt 7 ps 1.5 lc rgb throughput_color title 'Req/sec'

# === Plot 3: Latency breakdown ===
set title 'Latency Analysis' font 'Arial,14'
set ylabel 'Latency (ms)'
plot 'benchmark/results_20260208_191936.csv' using 1:3 skip 1 with linespoints lw 2 pt 7 lc rgb latency_color title 'Average', \
     'benchmark/results_20260208_191936.csv' using 1:5 skip 1 with linespoints lw 2 pt 11 lc rgb stdev_color title 'Std Dev'
     #'benchmark/results_20260208_191936.csv' using 1:4 skip 1 with linespoints lw 2 pt 9 lc rgb "#FF0000" title 'Maximum', \


# === Plot 4: Latency with error bars ===
set title 'Latency with Standard Deviation' font 'Arial,14'
set ylabel 'Latency (ms)'
plot 'benchmark/results_20260208_191936.csv' using 1:3:5 skip 1 with yerrorbars lw 2 pt 7 ps 1.5 lc rgb latency_color title 'Avg ± StdDev', \
     'benchmark/results_20260208_191936.csv' using 1:3 skip 1 with lines lw 2 lc rgb latency_color notitle

# === Plot 5: Transfer rate ===
set title 'Data Transfer Rate' font 'Arial,14'
set ylabel 'Transfer (MB/sec)'
set style fill solid 0.5
plot 'benchmark/results_20260208_191936.csv' using 1:6 skip 1 with filledcurves x1 lc rgb transfer_color notitle, \
     'benchmark/results_20260208_191936.csv' using 1:6 skip 1 with linespoints lw 3 pt 7 ps 1.5 lc rgb transfer_color title 'MB/sec'

# === Plot 6: Efficiency (req/sec per connection) ===
set title 'Connection Efficiency' font 'Arial,14'
set ylabel 'Requests per Connection'
plot 'benchmark/results_20260208_191936.csv' using 1:($2/$1) skip 1 with linespoints lw 3 pt 7 ps 1.5 lc rgb "#9900CC" title 'Req/sec per Connection'

unset multiplot
