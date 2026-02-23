/* conf.c - Configuration file parser for miniweb
 *
 * Lookup order (first found wins):
 *   1. Path from -f flag (CLI)
 *   2. ./miniweb.conf
 *   3. $HOME/.miniweb.conf
 *   4. /etc/miniweb.conf
 *
 * Format: key  value   (one per line, # comments, blank lines ignored)
 * CLI flags always override file values.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>

#include "../../include/conf.h"

void
conf_defaults(miniweb_conf_t *conf)
{
    memset(conf, 0, sizeof(*conf));

    /* Network */
    conf->port = 9001;
    strlcpy(conf->bind_addr, "127.0.0.1", sizeof(conf->bind_addr));

    /* Worker pool */
    conf->threads   = 4;
    conf->max_conns = 1280;

    /* Timeouts / limits */
    conf->conn_timeout    = 30;
    conf->max_req_size    = 16384;
    conf->mandoc_timeout  = 10;

    /* Filesystem */
    strlcpy(conf->static_dir,    "static",           sizeof(conf->static_dir));
    strlcpy(conf->templates_dir, "templates",        sizeof(conf->templates_dir));
    strlcpy(conf->mandoc_path,   "/usr/bin/mandoc",  sizeof(conf->mandoc_path));

    /* Reverse proxy */
    strlcpy(conf->trusted_proxy, "127.0.0.1", sizeof(conf->trusted_proxy));

    /* Logging */
    conf->verbose  = 0;
    conf->log_file[0] = '\0';
}

/* Strip leading whitespace; return pointer into s. */
static char *
ltrim(char *s)
{
    while (*s && isspace((unsigned char)*s))
        s++;
    return s;
}

/* Strip trailing whitespace in-place. */
static void
rtrim(char *s)
{
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1]))
        s[--n] = '\0';
}

/* Apply one key=value pair to conf.
 * Returns 0 on success, -1 if the key is unknown. */
static int
conf_apply_kv(miniweb_conf_t *conf, const char *key, const char *val)
{
    if (strcasecmp(key, "port") == 0) {
        conf->port = atoi(val);
    } else if (strcasecmp(key, "bind_addr") == 0 ||
               strcasecmp(key, "bind") == 0) {
        strlcpy(conf->bind_addr, val, sizeof(conf->bind_addr));
    } else if (strcasecmp(key, "threads") == 0) {
        conf->threads = atoi(val);
    } else if (strcasecmp(key, "max_conns") == 0) {
        conf->max_conns = atoi(val);
    } else if (strcasecmp(key, "conn_timeout") == 0) {
        conf->conn_timeout = atoi(val);
    } else if (strcasecmp(key, "max_req_size") == 0) {
        conf->max_req_size = atoi(val);
    } else if (strcasecmp(key, "mandoc_timeout") == 0) {
        conf->mandoc_timeout = atoi(val);
    } else if (strcasecmp(key, "static_dir") == 0) {
        strlcpy(conf->static_dir, val, sizeof(conf->static_dir));
    } else if (strcasecmp(key, "templates_dir") == 0) {
        strlcpy(conf->templates_dir, val, sizeof(conf->templates_dir));
    } else if (strcasecmp(key, "mandoc_path") == 0) {
        strlcpy(conf->mandoc_path, val, sizeof(conf->mandoc_path));
    } else if (strcasecmp(key, "trusted_proxy") == 0) {
        strlcpy(conf->trusted_proxy, val, sizeof(conf->trusted_proxy));
    } else if (strcasecmp(key, "verbose") == 0) {
        if (strcasecmp(val, "yes") == 0 || strcasecmp(val, "true") == 0)
            conf->verbose = 1;
        else if (strcasecmp(val, "no") == 0 || strcasecmp(val, "false") == 0)
            conf->verbose = 0;
        else
            conf->verbose = atoi(val);
    } else if (strcasecmp(key, "log_file") == 0) {
        strlcpy(conf->log_file, val, sizeof(conf->log_file));
    } else {
        return -1;
    }
    return 0;
}

/* Try to open and parse a single config file.
 * Returns  1  if the file does not exist (non-fatal, keep looking).
 * Returns  0  on success.
 * Returns -1  on parse error. */
static int
conf_parse_file(const char *path, miniweb_conf_t *conf)
{
    FILE  *fp;
    char   line[512];
    int    lineno = 0;

    fp = fopen(path, "r");
    if (fp == NULL)
        return 1; /* file not found */

    while (fgets(line, sizeof(line), fp) != NULL) {
        char *p, *key, *val;

        lineno++;
        p = ltrim(line);
        rtrim(p);

        /* Skip blank lines and comments */
        if (*p == '\0' || *p == '#')
            continue;

        /* Split on first whitespace */
        key = p;
        val = key;
        while (*val && !isspace((unsigned char)*val))
            val++;
        if (*val == '\0') {
            fprintf(stderr, "%s:%d: missing value for key '%s'\n",
                    path, lineno, key);
            fclose(fp);
            return -1;
        }
        *val++ = '\0';
        val = ltrim(val);

        if (conf_apply_kv(conf, key, val) != 0) {
            fprintf(stderr, "%s:%d: unknown key '%s'\n", path, lineno, key);
            fclose(fp);
            return -1;
        }
    }

    fclose(fp);
    return 0;
}

int
conf_load(const char *path, miniweb_conf_t *conf)
{
    int rc;

    if (path != NULL) {
        /* Explicit -f path: failure is fatal */
        rc = conf_parse_file(path, conf);
        if (rc == 1) {
            fprintf(stderr, "conf_load: config file not found: %s\n", path);
            return -1;
        }
        return (rc == 0) ? 0 : -1;
    }

    /* Lookup order */
    static const char *candidates[] = {
        "./miniweb.conf",
        NULL, /* $HOME/.miniweb.conf — filled below */
        "/etc/miniweb.conf",
    };

    char home_path[CONF_STR_MAX];
    const char *home = getenv("HOME");
    if (home != NULL) {
        snprintf(home_path, sizeof(home_path), "%s/.miniweb.conf", home);
        candidates[1] = home_path;
    }

    for (int i = 0; i < 3; i++) {
        if (candidates[i] == NULL)
            continue;
        rc = conf_parse_file(candidates[i], conf);
        if (rc == 0)
            return 0;  /* found and parsed */
        if (rc == -1)
            return -1; /* parse error */
        /* rc == 1: not found, try next */
    }

    /* No config file found — fine, defaults stand */
    return 0;
}

void
conf_apply_cli(miniweb_conf_t *conf,
               int cli_port,
               const char *cli_bind,
               int cli_threads,
               int cli_max_conns,
               const char *cli_log_file,
               int cli_verbose)
{
    if (cli_port    > 0)   conf->port      = cli_port;
    if (cli_bind   != NULL) strlcpy(conf->bind_addr, cli_bind, sizeof(conf->bind_addr));
    if (cli_threads > 0)   conf->threads   = cli_threads;
    if (cli_max_conns > 0) conf->max_conns = cli_max_conns;
    if (cli_log_file != NULL) strlcpy(conf->log_file, cli_log_file, sizeof(conf->log_file));
    if (cli_verbose)        conf->verbose   = cli_verbose;
}

void
conf_dump(const miniweb_conf_t *conf)
{
    fprintf(stderr, "=== miniweb active configuration ===\n");
    fprintf(stderr, "  port          : %d\n",  conf->port);
    fprintf(stderr, "  bind_addr     : %s\n",  conf->bind_addr);
    fprintf(stderr, "  threads       : %d\n",  conf->threads);
    fprintf(stderr, "  max_conns     : %d\n",  conf->max_conns);
    fprintf(stderr, "  conn_timeout  : %d\n",  conf->conn_timeout);
    fprintf(stderr, "  max_req_size  : %d\n",  conf->max_req_size);
    fprintf(stderr, "  mandoc_timeout: %d\n",  conf->mandoc_timeout);
    fprintf(stderr, "  static_dir    : %s\n",  conf->static_dir);
    fprintf(stderr, "  templates_dir : %s\n",  conf->templates_dir);
    fprintf(stderr, "  mandoc_path   : %s\n",  conf->mandoc_path);
    fprintf(stderr, "  trusted_proxy : %s\n",  conf->trusted_proxy);
    fprintf(stderr, "  verbose       : %d\n",  conf->verbose);
    fprintf(stderr, "  log_file      : %s\n",  conf->log_file[0] ? conf->log_file : "(stderr)");
    fprintf(stderr, "=====================================\n");
}
