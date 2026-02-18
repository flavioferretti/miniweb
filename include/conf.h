/* conf.h - Configuration file parser for miniweb
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

#ifndef CONF_H
#define CONF_H

#define CONF_STR_MAX  256   /* max length for string values */

/* All configurable knobs in one place.
 * Mirrors server_config in main.c but is the authoritative definition —
 * main.c will use this struct directly. */
typedef struct miniweb_conf {
    /* Network */
    int  port;                      /* -p  default: 9001           */
    char bind_addr[CONF_STR_MAX];   /* -b  default: "127.0.0.1"   */

    /* Worker pool */
    int  threads;                   /* -t  default: 4              */
    int  max_conns;                 /* -c  default: 1280           */

    /* Timeouts / limits */
    int  conn_timeout;              /*     default: 30  (seconds)  */
    int  max_req_size;              /*     default: 16384 (bytes)  */
    int  mandoc_timeout;            /*     default: 10  (seconds)  */

    /* Filesystem */
    char static_dir[CONF_STR_MAX];    /*   default: "static"       */
    char templates_dir[CONF_STR_MAX]; /*   default: "templates"    */
    char mandoc_path[CONF_STR_MAX];   /*   default: "/usr/bin/mandoc" */

    /* Reverse proxy */
    char trusted_proxy[CONF_STR_MAX]; /*   default: "127.0.0.1"
    *   X-Forwarded-* headers only
    *   accepted from this IP   */

    /* Logging */
    int  verbose;                   /* -v  default: 0              */
} miniweb_conf_t;

/* Fill *conf with compiled-in defaults. */
void conf_defaults(miniweb_conf_t *conf);

/* Try to load a config file.
 *   path  — explicit path from -f flag, or NULL to use lookup order.
 *   conf  — must already contain defaults (call conf_defaults first).
 *
 * Returns  0  on success or "file not found" (non-fatal).
 * Returns -1  on parse error (fatal — caller should exit).
 *
 * Only keys present in the file are overwritten; the rest keep their
 * current values (defaults or values already set by a previous call).
 */
int conf_load(const char *path, miniweb_conf_t *conf);

/* Apply CLI overrides onto *conf.
 * Pass -1 / NULL for any argument that was not supplied on the CLI. */
void conf_apply_cli(miniweb_conf_t *conf,
                    int cli_port,
                    const char *cli_bind,
                    int cli_threads,
                    int cli_max_conns,
                    int cli_verbose);

/* Print the active configuration to stderr (verbose mode). */
void conf_dump(const miniweb_conf_t *conf);

#endif /* CONF_H */
