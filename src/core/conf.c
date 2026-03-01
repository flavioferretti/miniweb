/* conf.c - Configuration file parser for miniweb */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include <miniweb/core/conf.h>

#include "conf_internal.h"

static char *
ltrim(char *s)
{
	while (*s && isspace((unsigned char)*s))
		s++;
	return s;
}

static void
rtrim(char *s)
{
	size_t n = strlen(s);
	while (n > 0 && isspace((unsigned char)s[n - 1]))
		s[--n] = '\0';
}

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

static int
conf_parse_line(miniweb_conf_t *conf, const char *path, int lineno, char *line)
{
	char *p;
	char *key;
	char *val;

	p = ltrim(line);
	rtrim(p);
	if (*p == '\0' || *p == '#')
		return 0;

	key = p;
	val = key;
	while (*val && !isspace((unsigned char)*val))
		val++;
	if (*val == '\0') {
		fprintf(stderr, "%s:%d: missing value for key '%s'\n", path, lineno, key);
		return -1;
	}
	*val++ = '\0';
	val = ltrim(val);

	if (conf_apply_kv(conf, key, val) != 0) {
		fprintf(stderr, "%s:%d: unknown key '%s'\n", path, lineno, key);
		return -1;
	}
	return 0;
}

static int
conf_parse_file(const char *path, miniweb_conf_t *conf)
{
	FILE *fp;
	char line[512];
	int lineno = 0;

	fp = fopen(path, "r");
	if (fp == NULL)
		return 1;

	while (fgets(line, sizeof(line), fp) != NULL) {
		lineno++;
		if (conf_parse_line(conf, path, lineno, line) != 0) {
			fclose(fp);
			return -1;
		}
	}

	if (conf_validate(conf) != 0) {
		fprintf(stderr, "%s: invalid configuration values\n", path);
		fclose(fp);
		return -1;
	}

	fclose(fp);
	return 0;
}

int
conf_load(const char *path, miniweb_conf_t *conf)
{
	int rc;

	if (path != NULL) {
		rc = conf_parse_file(path, conf);
		if (rc == 1) {
			fprintf(stderr, "conf_load: config file not found: %s\n", path);
			return -1;
		}
		return (rc == 0) ? 0 : -1;
	}

	const char *candidates[] = {
		"./miniweb.conf",
		NULL,
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
			return 0;
		if (rc == -1)
			return -1;
	}

	return 0;
}
