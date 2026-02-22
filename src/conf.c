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

#include "../include/log.h"

/* -- Helpers ---------------------------------------------------------------- */

/* Trim leading whitespace in-place; return pointer to first non-space char. */
/**
 * @brief Ltrim.
 * @param s Input string to parse or sanitize.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
static char *
ltrim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

/* Trim trailing whitespace in-place. */
/**
 * @brief Rtrim.
 * @param s Input string to parse or sanitize.
 */
static void
rtrim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

/* Parse a bounded integer; return 0 on success, -1 on error. */
/**
 * @brief Parse int.
 * @param s Input string to parse or sanitize.
 * @param out Output pointer for parsed or generated value.
 * @param lo Inclusive minimum accepted value.
 * @param hi Inclusive maximum accepted value.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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

/* -- Defaults --------------------------------------------------------------- */
/**
 * @brief Conf defaults.
 * @param conf Configuration object to populate or inspect.
 */
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
    conf->log_file[0]    = '\0';

    strlcpy(conf->bind_addr,     "127.0.0.1",         sizeof(conf->bind_addr));
    strlcpy(conf->static_dir,    "static",             sizeof(conf->static_dir));
    strlcpy(conf->templates_dir, "templates",          sizeof(conf->templates_dir));
    strlcpy(conf->mandoc_path,   "/usr/bin/mandoc",    sizeof(conf->mandoc_path));
    strlcpy(conf->trusted_proxy, "127.0.0.1",          sizeof(conf->trusted_proxy));
}

/* -- File lookup ------------------------------------------------------------ */

/* Try to open a file; return FILE* or NULL without setting errno. */
/**
 * @brief Try open.
 * @param path Request or filesystem path to evaluate.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
static FILE *
try_open(const char *path)
{
    if (!path || path[0] == '\0')
        return NULL;
    return fopen(path, "r");
}

/* Build $HOME/.miniweb.conf into buf[len]; return 0 on success. */
/**
 * @brief Home conf path.
 * @param buf Input buffer containing textual data.
 * @param len Destination buffer length.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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

/* -- Core parser ------------------------------------------------------------ */
/**
 * @brief Parse file.
 * @param f Input stream to parse.
 * @param path Request or filesystem path to evaluate.
 * @param conf Configuration object to populate or inspect.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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
            log_error("%s:%d: missing value for key '%s'", path, lineno, key);
            errors++;
            continue;
        }

        /* Trim any trailing whitespace/comment from value */
        rtrim(val);

        /* -- Dispatch -- */
        #define KSTR(name, field) \
        if (strcasecmp(key, name) == 0) { \
            strlcpy(conf->field, val, sizeof(conf->field)); continue; }

            #define KINT(name, field, lo, hi) \
            if (strcasecmp(key, name) == 0) { \
                int _v; \
                if (parse_int(val, &_v, lo, hi) != 0) { \
                    log_error("%s:%d: invalid value for '%s': %s (must be %d-%d)", path, lineno, key, val, lo, hi); \
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
                            log_error("%s:%d: invalid boolean for '%s': %s", path, lineno, key, val); \
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
            KSTR ("log_file",        log_file)
            KBOOL("verbose",         verbose)
            KSTR ("log_file",        log_file)

            #undef KSTR
            #undef KINT
            #undef KBOOL

            /* Unknown key */
            log_error("%s:%d: unknown key '%s' (ignored)", path, lineno, key);
    }

    return errors ? -1 : 0;
}

/* -- Public API ------------------------------------------------------------- */
/**
 * @brief Conf load.
 * @param explicit_path Parameter used by this function.
 * @param conf Configuration object to populate or inspect.
 * @return Returns 0 on success or a negative value on failure unless documented otherwise.
 */
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
            log_error("conf: cannot open '%s': %s", explicit_path, strerror(errno));
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

    log_info("conf: loading %s", used);
    int rc = parse_file(f, used, conf);
    fclose(f);
    return rc;
}

/**
 * @brief Apply CLI overrides to loaded configuration values.
 * @param conf Configuration object to mutate.
 * @param cli_port Port override value.
 * @param cli_bind Bind address override value.
 * @param cli_threads Worker thread count override value.
 * @param cli_max_conns Maximum concurrent connections override value.
 * @param cli_verbose Non-zero to force verbose logging.
 */
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

/**
 * @brief Conf dump.
 * @param conf Configuration object to populate or inspect.
 */
void
conf_dump(const miniweb_conf_t *conf)
{
    log_info("conf: active configuration");
    log_info("  port            %d", conf->port);
    log_info("  bind            %s", conf->bind_addr);
    log_info("  threads         %d", conf->threads);
    log_info("  max_conns       %d", conf->max_conns);
    log_info("  conn_timeout    %d s", conf->conn_timeout);
    log_info("  max_req_size    %d bytes", conf->max_req_size);
    log_info("  mandoc_timeout  %d s", conf->mandoc_timeout);
    log_info("  static_dir      %s", conf->static_dir);
    log_info("  templates_dir   %s", conf->templates_dir);
    log_info("  mandoc_path     %s", conf->mandoc_path);
    log_info("  trusted_proxy   %s", conf->trusted_proxy);
    log_info("  verbose         %s", conf->verbose ? "yes" : "no");
    log_info("  log_file        %s", conf->log_file[0] ? conf->log_file : "(stderr)");

}
