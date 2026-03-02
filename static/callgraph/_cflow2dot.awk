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
    if (MAX_EDGES == "") MAX_EDGES = 5000   # limite sicurezza reverse graph

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
