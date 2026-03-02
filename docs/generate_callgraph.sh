#!/bin/ksh
# generate_callgraph.sh  —  Call-graph generator for the miniweb project
#
# Strategy
# --------
#  1. Run cflow on all .c sources to get the real call tree.
#  2. Parse cflow's indented text output with awk (no fragile regex hacks).
#  3. Emit several DOT files with proper visual styling and render to SVG.
#  4. Build a standalone HTML viewer that uses the project's own CSS.
#
# Dependencies: cflow  graphviz(dot/fdp)
#   OpenBSD:  pkg_add cflow graphviz
#
# Location: miniweb/docs/generate_callgraph.sh

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
PROJECT_ROOT="/home/$USER/DEV/miniweb"
SRC_DIR="$PROJECT_ROOT/src"
INCLUDE_DIR="$PROJECT_ROOT/include"
OUTPUT_DIR="$PROJECT_ROOT/static/callgraph"

# Lower depth (avoid CPU saturation)
COMPACT_DEPTH=3

# ---------------------------------------------------------------------------
# Terminal colours (only on a real tty)
# ---------------------------------------------------------------------------
NORMAL=''; GREEN=''; YELLOW=''; BLUE=''
if test -t 1; then
    GREEN='\033[0;32m'; YELLOW='\033[1;33m'
    BLUE='\033[0;34m';  NORMAL='\033[0m'
fi

info() { print "${BLUE}>>> $*${NORMAL}"; }
ok()   { print "${GREEN}    OK  $*${NORMAL}"; }
warn() { print "${YELLOW}    WARN $*${NORMAL}"; }
die()  { print "ERROR: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# Pre-flight checks
# ---------------------------------------------------------------------------
command -v cflow >/dev/null 2>&1 || die "cflow not found — run: pkg_add cflow"
command -v dot   >/dev/null 2>&1 || die "dot not found  — run: pkg_add graphviz"

mkdir -p "$OUTPUT_DIR"
cd "$PROJECT_ROOT" || die "cannot cd to $PROJECT_ROOT"

info "=== miniweb Call Graph Generator ==="
print "  Source dir : $SRC_DIR"
print "  Output dir : $OUTPUT_DIR"
print "  Compact depth : $COMPACT_DEPTH"
print ""

# ---------------------------------------------------------------------------
# 1. Collect all .c source files
# ---------------------------------------------------------------------------
info "Collecting .c source files..."
C_FILES=""
C_COUNT=0
for f in $(find "$SRC_DIR" -type f -name "*.c" | sort); do
    C_FILES="$C_FILES $f"
    C_COUNT=$((C_COUNT + 1))
done
print "    Found $C_COUNT .c files"
[ "$C_COUNT" -eq 0 ] && die "No .c files found in $SRC_DIR"

CFLOW_OPTS="-I $INCLUDE_DIR"

# ---------------------------------------------------------------------------
# 2. Run cflow and save raw text outputs
# ---------------------------------------------------------------------------
info "Running cflow (full tree, depth unlimited)..."
cflow -d 100 $CFLOW_OPTS $C_FILES \
    > "$OUTPUT_DIR/callgraph_full.txt" 2>/dev/null
ok "Saved: callgraph_full.txt"

info "Running cflow from main() entry point (full depth)..."
cflow -d 100 -m main $CFLOW_OPTS $C_FILES \
    > "$OUTPUT_DIR/callgraph_from_main.txt" 2>/dev/null
ok "Saved: callgraph_from_main.txt"

info "Running cflow (compact: depth $COMPACT_DEPTH from main)..."
cflow -d $COMPACT_DEPTH -m main $CFLOW_OPTS $C_FILES \
    > "$OUTPUT_DIR/callgraph_compact.txt" 2>/dev/null
ok "Saved: callgraph_compact.txt"

# ---------------------------------------------------------------------------
# 3. AWK parser: cflow indented tree -> DOT
#
# GNU cflow default output (same format used by the OpenBSD port):
#
#   main() <int main (int argc, char **argv) at app_main.c:108>:
#       parse_args() <void parse_args () at app_main.c:61>:
#           conf_defaults() <void conf_defaults () at conf_defaults.c:5>
#           conf_load() <int conf_load () at conf.c:296>:
#               conf_validate() <int conf_validate () at conf_validation.c:5>
#
# Rules:
#   * Indentation unit is auto-detected from the first indented line
#     (works whether cflow uses 2, 4, or 8 spaces).
#   * Function name = everything before the first "(" on the line.
#   * A depth stack maps depth -> current function name.
#   * Edge "parent -> child" is emitted once per unique pair.
#   * In "compact" style, common libc/pthread symbols are filtered out.
# ---------------------------------------------------------------------------
CFLOW_TO_DOT_AWK="$OUTPUT_DIR/_cflow2dot.awk"

cat > "$CFLOW_TO_DOT_AWK" << 'AWKEOF'
# cflow2dot.awk  —  convert GNU cflow default indented output to DOT.
#
# Variables (set with -v on the awk command line):
#   TITLE      string embedded in a DOT comment
#   STYLE      "full" | "compact"
#              compact filters out stdlib / pthread / libc symbols
#   MAX_DEPTH  integer, skip lines deeper than this (default: unlimited)
#   MAX_EDGES  integer, stop after this many edges (prevents huge graphs)

BEGIN {
    indent_unit = 0
    if (MAX_DEPTH == "") MAX_DEPTH = 99999
    if (MAX_EDGES == "") MAX_EDGES = 5000

    print "digraph callgraph {"
    print "  // " TITLE
    print "  rankdir=TB;"
    print "  bgcolor=\"#1e1e2e\";"
    print "  fontname=\"Monaco\";"
    print "  nodesep=0.55;"
    print "  ranksep=0.8;"
    print "  splines=curved;"
    print ""
    print "  node [shape=box, style=\"filled,rounded\", fontname=\"Monaco\","
    print "        fontsize=9, fillcolor=\"#313244\", color=\"#585b70\","
    print "        fontcolor=\"#cdd6f4\", penwidth=1.2];"
    print "  edge [color=\"#585b70\", arrowhead=normal, penwidth=1.0,"
    print "        fontname=\"Monaco\", fontsize=7];"
    print ""
}

{
    # Stop se abbiamo raggiunto il limite di edges
    if (edge_count >= MAX_EDGES) next

    line = $0

    # Count leading spaces to determine call depth
    spaces = 0
    while (substr(line, spaces + 1, 1) == " ") spaces++

    # Auto-detect the indent unit from the first indented line
    if (spaces > 0 && indent_unit == 0) indent_unit = spaces

    depth = (indent_unit > 0) ? int(spaces / indent_unit) : 0

    # Skip lines deeper than the requested maximum
    if (depth + 0 > MAX_DEPTH + 0) next

    # Extract function name: everything before the first "("
    rest = substr(line, spaces + 1)
    paren = index(rest, "(")
    if (paren == 0) next
    fname = substr(rest, 1, paren - 1)
    gsub(/^[ \t]+|[ \t]+$/, "", fname)
    if (fname == "") next

    # In compact mode, skip common libc / POSIX / pthread symbols
    if (STYLE == "compact") {
        if (fname ~ /^(printf|fprintf|sprintf|snprintf|vfprintf|fopen|fclose|fgets|fread|fwrite|fputs|fputc|fflush|malloc|free|calloc|realloc|memset|memcpy|memmove|strlen|strcpy|strncpy|strlcpy|strcmp|strncmp|strcasecmp|strncasecmp|strchr|strrchr|strstr|strtok|strtok_r|strtol|atoi|atol|getenv|getopt|time|clock_gettime|localtime_r|strftime|pthread_create|pthread_join|pthread_mutex_lock|pthread_mutex_unlock|pthread_mutex_init|pthread_cond_wait|pthread_cond_timedwait|pthread_cond_signal|pthread_cond_broadcast|pthread_cond_init|pthread_condattr_init|pthread_condattr_setclock|pthread_condattr_destroy|pthread_once|sem_init|sem_wait|sem_post|sem_destroy|read|write|close|open|pipe|fork|execv|waitpid|kill|poll|dup|dup2|fcntl|accept|bind|listen|socket|setsockopt|connect|stat|fstat|lstat|mkdir|unlink|rename|opendir|closedir|readdir|isspace|isdigit|isalpha|tolower|toupper|strerror|abort|exit|_exit|signal|sigaction|sigemptyset|sigaddset|inet_pton|inet_ntop|htons|htonl|ntohs|ntohl)$/)
            next
    }

    # Maintain a depth stack; clear stale entries when depth decreases
    stack[depth] = fname
    for (d = depth + 1; d <= 128; d++) stack[d] = ""

    # Emit a unique edge from parent to this node
    if (depth > 0 && stack[depth - 1] != "") {
        parent = stack[depth - 1]
        key    = parent SUBSEP fname
        if (!(key in seen)) {
            seen[key] = 1
            printf "  \"%s\" -> \"%s\";\n", parent, fname
            edge_count++
        }
    }
}

END {
    if (edge_count >= 5000) {
        print "  // NOTA: Raggiunto limite di 5000 edges per performance"
    }
    print ""
    print "  // Entry-point node"
    print "  main [fillcolor=\"#89b4fa\", color=\"#1e1e2e\","
    print "        fontcolor=\"#1e1e2e\", penwidth=2, shape=doublecircle];"
    print "}"
}
AWKEOF
ok "AWK parser written: _cflow2dot.awk"

# ---------------------------------------------------------------------------
# 4. Convert each cflow text output to a DOT file
# ---------------------------------------------------------------------------
info "Converting cflow output to DOT files..."

awk -f "$CFLOW_TO_DOT_AWK" \
    -v TITLE="miniweb — full call graph (all functions)" \
    -v STYLE="full" \
    -v MAX_DEPTH=100 \
    -v MAX_EDGES=5000 \
    "$OUTPUT_DIR/callgraph_full.txt" \
    > "$OUTPUT_DIR/callgraph_full.dot"
ok "Saved: callgraph_full.dot"

awk -f "$CFLOW_TO_DOT_AWK" \
    -v TITLE="miniweb — call graph from main()" \
    -v STYLE="full" \
    -v MAX_DEPTH=100 \
    -v MAX_EDGES=5000 \
    "$OUTPUT_DIR/callgraph_from_main.txt" \
    > "$OUTPUT_DIR/callgraph_main.dot"
ok "Saved: callgraph_main.dot"

awk -f "$CFLOW_TO_DOT_AWK" \
    -v TITLE="miniweb — compact call graph (depth $COMPACT_DEPTH, no stdlib)" \
    -v STYLE="compact" \
    -v MAX_DEPTH=$COMPACT_DEPTH \
    -v MAX_EDGES=2000 \
    "$OUTPUT_DIR/callgraph_compact.txt" \
    > "$OUTPUT_DIR/callgraph_compact.dot"
ok "Saved: callgraph_compact.dot"


# ---------------------------------------------------------------------------
# 5. Render SVG files from each DOT (con timeout)
# ---------------------------------------------------------------------------
info "Rendering SVG files (con timeout 30 secondi per grafico)..."

render_svg() {
    local dotfile="$1"
    local svgfile="${dotfile%.dot}.svg"
    local layout="${2:-dot}"
    local timeout=30

    print "    Rendering $(basename "$dotfile") with $layout (max ${timeout}s)..."

    # Use timeout if present, otherwise run directly
    if command -v timeout >/dev/null 2>&1; then
        if timeout $timeout "$layout" -Tsvg "$dotfile" -o "$svgfile" 2>/dev/null; then
            ok "Saved: $(basename "$svgfile")  [layout: $layout]"
        else
            warn "Render failed or timeout: $(basename "$dotfile")"
        fi
    else
        # On OpenBSD timeout maybe not present
        "$layout" -Tsvg "$dotfile" -o "$svgfile" 2>/dev/null && \
            ok "Saved: $(basename "$svgfile")  [layout: $layout]" || \
            warn "Render failed: $(basename "$dotfile")"
    fi
}

# Compact graphs: dot (hierarchical) works best
render_svg "$OUTPUT_DIR/callgraph_compact.dot"  dot

# From-main full-depth: dot is still cleaner than fdp here
render_svg "$OUTPUT_DIR/callgraph_main.dot"     dot

# Full graph: force-directed (fdp) handles very dense graphs better than dot
if command -v fdp >/dev/null 2>&1; then
    render_svg "$OUTPUT_DIR/callgraph_full.dot" fdp
else
    render_svg "$OUTPUT_DIR/callgraph_full.dot" dot
fi

# Extra: compact graph in left-to-right orientation
dot -Grankdir=LR -Tsvg "$OUTPUT_DIR/callgraph_compact.dot" \
    -o "$OUTPUT_DIR/callgraph_compact_lr.svg" 2>/dev/null && \
    ok "Saved: callgraph_compact_lr.svg  [layout: dot LR]"

# ---------------------------------------------------------------------------
# 6. Function inventory (grep-based, quick cross-reference)
# ---------------------------------------------------------------------------
info "Building function inventory..."

{
    printf "# miniweb — public function inventory per source file\n"
    printf "# Generated by generate_callgraph.sh\n"
    printf "# Format:  src/path/file.c: func1, func2, ...\n\n"
    for cfile in $(find "$SRC_DIR" -name "*.c" | sort); do
        rel="${cfile#$PROJECT_ROOT/}"
        funcs=$(grep -E '^[a-zA-Z_][a-zA-Z0-9_ *]+ [a-zA-Z_][a-zA-Z0-9_]+[[:space:]]*\(' \
            "$cfile" 2>/dev/null | \
            grep -v '^static ' | grep -v '//' | \
            sed 's/(.*//' | awk '{print $NF}' | \
            sort -u | tr '\n' ',' | sed 's/,$//')
        [ -n "$funcs" ] && printf "%s: %s\n" "$rel" "$funcs"
    done
} > "$OUTPUT_DIR/function_inventory.txt"
ok "Saved: function_inventory.txt"

# ---------------------------------------------------------------------------
# 7. HTML viewer
# ---------------------------------------------------------------------------
info "Generating HTML viewer..."

cat > "$OUTPUT_DIR/callgraph.html" << '_HTML_'
<!DOCTYPE html>
<html lang="en" data-theme="dark">
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>miniweb — Call Graph</title>
  <link rel="stylesheet" href="/static/css/custom.css">
  <style>
    /* ... (stile invariato) ... */
  </style>
</head>
<body style="background-color: var(--bg);">
<div class="container">
    <h1>📊 MiniWeb — Call Graph Analysis</h1>
    <p>Progetto: <code>/home/flavio/DEV/miniweb</code> &nbsp;|&nbsp; Generato: <span id="date"></span></p>

    <nav>
        <a href="#compact">📦 Call Graph Compatto</a>
        <a href="#compact-lr">↔️ Compatto Orizzontale</a>
        <a href="#main">🎯 Call Graph da main()</a>
        <a href="#files">📄 File</a>
    </nav>

    <h2 id="compact">📦 Call Graph Compatto (profondità 3, senza librerie di sistema) <span class="badge">SVG</span></h2>
    <p>Vista semplificata che mostra le relazioni principali tra le funzioni del progetto, filtrando le chiamate alle librerie standard.</p>
    <div class="graph-wrap">
        <object type="image/svg+xml" data="callgraph_compact.svg"></object>
    </div>

    <h2 id="compact-lr">↔️ Call Graph Compatto — Layout Orizzontale <span class="badge">SVG</span></h2>
    <p>La stessa vista compatta ma con orientamento orizzontale, utile per grafi molto larghi.</p>
    <div class="graph-wrap">
        <object type="image/svg+xml" data="callgraph_compact_lr.svg"></object>
    </div>

    <h2 id="main">🎯 Call Graph a partire da main() <span class="badge">SVG</span></h2>
    <p>Mostra tutte le funzioni raggiungibili dal main, con profondità illimitata. Utile per capire il flusso principale dell'applicazione.</p>
    <div class="graph-wrap">
        <object type="image/svg+xml" data="callgraph_main.svg"></object>
    </div>

    <h2 id="files">📄 File sorgente generati</h2>
    <div class="file-grid">
        <a href="callgraph_compact.dot">callgraph_compact.dot</a>
        <a href="callgraph_main.dot">callgraph_main.dot</a>
        <a href="callgraph_compact.svg">callgraph_compact.svg</a>
        <a href="callgraph_compact_lr.svg">callgraph_compact_lr.svg</a>
        <a href="callgraph_main.svg">callgraph_main.svg</a>
        <a href="function_inventory.txt">function_inventory.txt</a>
        <a href="_cflow2dot.awk">_cflow2dot.awk (parser AWK)</a>
        <a href="callgraph_full.txt">callgraph_full.txt (raw cflow)</a>
        <a href="callgraph_from_main.txt">callgraph_from_main.txt (raw cflow)</a>
        <a href="callgraph_compact.txt">callgraph_compact.txt (raw cflow)</a>
    </div>

    <footer>
        <p>🔧 Generato con cflow + graphviz | <code>./docs/generate_callgraph.sh</code></p>
        <p>I grafi DOT sono modificabili manualmente: <code>dot -Tsvg foo.dot -o foo.svg</code></p>
        <p>📊 Grafici disponibili: compatti (3 livelli) e da main (profondità illimitata)</p>
    </footer>
</div>
<script>
    document.getElementById('date').textContent = new Date().toLocaleString('it-IT');
</script>
</body>
</html>
_HTML_
ok "Saved: callgraph.html"

# ---------------------------------------------------------------------------
# 8. Summary
# ---------------------------------------------------------------------------
print ""
info "=== All done ==="
print "${GREEN}Output:${NORMAL}  $OUTPUT_DIR"
print "${GREEN}Viewer:${NORMAL}  http://localhost:9001/callgraph/"
print ""
print "Files:"
ls -lh "$OUTPUT_DIR" 2>/dev/null | \
    awk 'NR>1 {printf "  %-46s %6s\n", $NF, $5}'
print ""
print "Performance note:"
print "  - Compact depth reduced from 4 to $COMPACT_DEPTH"
print "  - Limit edges: 5000 full"
print "  - Timeout 30 seconds for rendering"
print ""
print "Tip — if chartis too big, use:"
print "  grep -c '->' *.dot  # count relations"
print "  modify MAX_EDGES accordingly into the script"
