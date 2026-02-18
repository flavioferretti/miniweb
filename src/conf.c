/* conf.c - Configuration file parser for miniweb
 *
 * Format rules:
 *   - One directive per line: key<whitespace>value
 *   - Lines beginning with '#' (after optional whitespace) are comments
 *   - Blank lines are ignored
 *   - Keys are case-insensitive
 *   - String values may NOT be quoted — keep paths simple
 *   - Unknown keys emit a warning but do not abort parsing
 */

#include "conf.h"

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Helpers ──────────────────────────────────────────────────────────────── */

/* Trim leading whitespace in-place; return pointer to first non-space char. */
static char *
ltrim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

/* Trim trailing whitespace in-place. */
static void
rtrim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

/* Parse a bounded integer; return 0 on success, -1 on error. */
static int
parse_int(const char *s, int *out, int lo, int hi)
{
    char *end;
    errno = 0;
    long v = strtol(s, &end, 10);
    if (errno != 0 || end == s || *end != '\0')
        return -1;
    if (v < lo || v > hi)
        return -1;
    *out = (int)v;
    return 0;
}

/* ── Defaults ─────────────────────────────────────────────────────────────── */
void
conf_defaults(miniweb_conf_t *conf)
{
    memset(conf, 0, sizeof(*conf));

    conf->port           = 9001;
    conf->threads        = 4;
    conf->max_conns      = 1280;
    conf->conn_timeout   = 30;
    conf->max_req_size   = 16384;
    conf->mandoc_timeout = 10;
    conf->verbose        = 0;

    strlcpy(conf->bind_addr,     "127.0.0.1",         sizeof(conf->bind_addr));
    strlcpy(conf->static_dir,    "static",             sizeof(conf->static_dir));
    strlcpy(conf->templates_dir, "templates",          sizeof(conf->templates_dir));
    strlcpy(conf->mandoc_path,   "/usr/bin/mandoc",    sizeof(conf->mandoc_path));
    strlcpy(conf->trusted_proxy, "127.0.0.1",          sizeof(conf->trusted_proxy));
}

/* ── File lookup ──────────────────────────────────────────────────────────── */

/* Try to open a file; return FILE* or NULL without setting errno. */
static FILE *
try_open(const char *path)
{
    if (!path || path[0] == '\0')
        return NULL;
    return fopen(path, "r");
}

/* Build $HOME/.miniweb.conf into buf[len]; return 0 on success. */
static int
home_conf_path(char *buf, size_t len)
{
    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        struct passwd *pw = getpwuid(getuid());
        if (!pw)
            return -1;
        home = pw->pw_dir;
    }
    int n = snprintf(buf, len, "%s/.miniweb.conf", home);
    return (n > 0 && (size_t)n < len) ? 0 : -1;
}

/* ── Core parser ──────────────────────────────────────────────────────────── */
static int
parse_file(FILE *f, const char *path, miniweb_conf_t *conf)
{
    char line[512];
    int  lineno = 0;
    int  errors = 0;

    while (fgets(line, sizeof(line), f)) {
        lineno++;
        rtrim(line);
        char *p = ltrim(line);

        /* Skip blank lines and comments */
        if (*p == '\0' || *p == '#')
            continue;

        /* Split on first whitespace */
        char key[64]  = {0};
        char val[CONF_STR_MAX] = {0};

        if (sscanf(p, "%63s %255[^\n]", key, val) < 2) {
            fprintf(stderr, "%s:%d: missing value for key '%s'\n",
                    path, lineno, key);
            errors++;
            continue;
        }

        /* Trim any trailing whitespace/comment from value */
        rtrim(val);

        /* ── Dispatch ── */
        #define KSTR(name, field) \
        if (strcasecmp(key, name) == 0) { \
            strlcpy(conf->field, val, sizeof(conf->field)); continue; }

            #define KINT(name, field, lo, hi) \
            if (strcasecmp(key, name) == 0) { \
                int _v; \
                if (parse_int(val, &_v, lo, hi) != 0) { \
                    fprintf(stderr, "%s:%d: invalid value for '%s': %s " \
                    "(must be %d–%d)\n", path, lineno, key, val, lo, hi); \
                    errors++; \
                } else { conf->field = _v; } \
                    continue; \
            }

            #define KBOOL(name, field) \
            if (strcasecmp(key, name) == 0) { \
                if (strcasecmp(val, "yes") == 0 || \
                    strcasecmp(val, "true") == 0 || \
                    strcmp(val, "1") == 0) \
                    conf->field = 1; \
                    else if (strcasecmp(val, "no") == 0 || \
                        strcasecmp(val, "false") == 0 || \
                        strcmp(val, "0") == 0) \
                        conf->field = 0; \
                        else { \
                            fprintf(stderr, "%s:%d: invalid boolean for '%s': %s\n", \
                            path, lineno, key, val); \
                            errors++; \
                        } \
                        continue; \
            }

            /* Network */
            KINT ("port",            port,           1,  65535)
            KSTR ("bind",            bind_addr)

            /* Workers */
            KINT ("threads",         threads,         1,  64)
            KINT ("max_conns",       max_conns,        1,  65535)

            /* Timeouts / limits */
            KINT ("conn_timeout",    conn_timeout,     1,  3600)
            KINT ("max_req_size",    max_req_size,     1024, 1048576)
            KINT ("mandoc_timeout",  mandoc_timeout,   1,  120)

            /* Filesystem */
            KSTR ("static_dir",      static_dir)
            KSTR ("templates_dir",   templates_dir)
            KSTR ("mandoc_path",     mandoc_path)

            /* Reverse proxy */
            KSTR ("trusted_proxy",   trusted_proxy)

            /* Logging */
            KBOOL("verbose",         verbose)

            #undef KSTR
            #undef KINT
            #undef KBOOL

            /* Unknown key */
            fprintf(stderr, "%s:%d: unknown key '%s' (ignored)\n",
                    path, lineno, key);
    }

    return errors ? -1 : 0;
}

/* ── Public API ───────────────────────────────────────────────────────────── */
int
conf_load(const char *explicit_path, miniweb_conf_t *conf)
{
    FILE       *f    = NULL;
    const char *used = NULL;
    char        homebuf[PATH_MAX];

    /* 1. Explicit path from -f */
    if (explicit_path) {
        f = try_open(explicit_path);
        if (!f) {
            fprintf(stderr, "conf: cannot open '%s': %s\n",
                    explicit_path, strerror(errno));
            return -1;   /* explicit path failing IS fatal */
        }
        used = explicit_path;
    }

    /* 2. ./miniweb.conf */
    if (!f) {
        f = try_open("./miniweb.conf");
        if (f) used = "./miniweb.conf";
    }

    /* 3. $HOME/.miniweb.conf */
    if (!f) {
        if (home_conf_path(homebuf, sizeof(homebuf)) == 0) {
            f = try_open(homebuf);
            if (f) used = homebuf;
        }
    }

    /* 4. /etc/miniweb.conf */
    if (!f) {
        f = try_open("/etc/miniweb.conf");
        if (f) used = "/etc/miniweb.conf";
    }

    /* No config file found — not an error, just use defaults/CLI */
    if (!f)
        return 0;

    fprintf(stderr, "conf: loading %s\n", used);
    int rc = parse_file(f, used, conf);
    fclose(f);
    return rc;
}

void
conf_apply_cli(miniweb_conf_t *conf,
               int cli_port, const char *cli_bind,
               int cli_threads, int cli_max_conns,
               int cli_verbose)
{
    if (cli_port    > 0)    conf->port           = cli_port;
    if (cli_bind)           strlcpy(conf->bind_addr, cli_bind,
        sizeof(conf->bind_addr));
    if (cli_threads > 0)    conf->threads        = cli_threads;
    if (cli_max_conns > 0)  conf->max_conns      = cli_max_conns;
    if (cli_verbose)        conf->verbose        = 1;
}

void
conf_dump(const miniweb_conf_t *conf)
{
    fprintf(stderr,
            "conf: active configuration:\n"
            "  port            %d\n"
            "  bind            %s\n"
            "  threads         %d\n"
            "  max_conns       %d\n"
            "  conn_timeout    %d s\n"
            "  max_req_size    %d bytes\n"
            "  mandoc_timeout  %d s\n"
            "  static_dir      %s\n"
            "  templates_dir   %s\n"
            "  mandoc_path     %s\n"
            "  trusted_proxy   %s\n"
            "  verbose         %s\n",
            conf->port,
            conf->bind_addr,
            conf->threads,
            conf->max_conns,
            conf->conn_timeout,
            conf->max_req_size,
            conf->mandoc_timeout,
            conf->static_dir,
            conf->templates_dir,
            conf->mandoc_path,
            conf->trusted_proxy,
            conf->verbose ? "yes" : "no");
}
