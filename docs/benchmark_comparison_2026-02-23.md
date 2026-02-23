# Benchmark comparison (2026-02-23 11:38 run vs baseline)

This note compares the benchmark snippet provided in chat (generated `2026-02-23 11:38:19`) against `static/benchmark_baseline.html` (generated `2026-02-23 10:23:14`).

## Headline result

The newer run is materially better overall:

- **Peak throughput:** `35341.94 req/s` vs baseline `34949.62 req/s` (**~+1.1%**).
- **Average throughput:** `15908.6 req/s` vs baseline `13071.1 req/s` (**~+21.7%**).
- **Best average latency:** `0.119 ms` vs baseline `0.156 ms` (**~23.7% lower is better**).
- **Worst max latency:** `1580.000 ms` vs baseline `2000.000 ms` (**~21.0% lower is better**).
- **Grand average latency:** `10.64 ms` vs baseline `68.07 ms` (**~84.4% lower is better**).

## Readout

- Overall behavior improved significantly in the newer run, especially latency stability across the full endpoint set.
- The new run still shows the same high-concurrency rough edge visible in baseline-style reports: several endpoints at `256` connections have `HTTP=000000` while row `Status` remains `OK` (likely a reporting quirk / partial-failure accounting in the benchmark parser rather than true all-success).
- Endpoint-level pattern from the snippet indicates best scaling around **128 connections** for multiple routes (for example static JS/API routes), while some HTML/manpage routes degrade faster as concurrency rises.

## Suggested next checks

1. Treat `HTTP=000000` rows as suspect in post-processing and cross-check with server logs for socket resets/timeouts.
2. Re-run with finer concurrency steps around `96/128/160` to find more stable saturation points per endpoint class.
3. Split reports by content type (static small files vs dynamic docs/manpages vs API) and tune worker/thread config per dominant workload.
