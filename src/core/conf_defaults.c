#include <stdio.h>
#include <string.h>

#include <miniweb/core/conf.h>

void
conf_defaults(miniweb_conf_t *conf)
{
	memset(conf, 0, sizeof(*conf));

	conf->port = 9001;
	strlcpy(conf->bind_addr, "127.0.0.1", sizeof(conf->bind_addr));

	conf->threads = 4;
	conf->max_conns = 1280;

	conf->conn_timeout = 30;
	conf->max_req_size = 16384;
	conf->mandoc_timeout = 10;

	strlcpy(conf->static_dir, "static", sizeof(conf->static_dir));
	strlcpy(conf->templates_dir, "templates", sizeof(conf->templates_dir));
	conf->autoindex = 0;
	strlcpy(conf->mandoc_path, "/usr/bin/mandoc", sizeof(conf->mandoc_path));

	strlcpy(conf->trusted_proxy, "127.0.0.1", sizeof(conf->trusted_proxy));

	conf->verbose = 0;
	conf->log_file[0] = '\0';

	conf->enable_views = 1;
	conf->enable_metrics = 1;
	conf->enable_networking = 1;
	conf->enable_man = 1;
	conf->enable_packages = 1;
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
	if (cli_port > 0)
		conf->port = cli_port;
	if (cli_bind != NULL)
		strlcpy(conf->bind_addr, cli_bind, sizeof(conf->bind_addr));
	if (cli_threads > 0)
		conf->threads = cli_threads;
	if (cli_max_conns > 0)
		conf->max_conns = cli_max_conns;
	if (cli_log_file != NULL)
		strlcpy(conf->log_file, cli_log_file, sizeof(conf->log_file));
	if (cli_verbose)
		conf->verbose = cli_verbose;
}

void
conf_dump(const miniweb_conf_t *conf)
{
	fprintf(stderr, "=== miniweb active configuration ===\n");
	fprintf(stderr, "  port          : %d\n", conf->port);
	fprintf(stderr, "  bind_addr     : %s\n", conf->bind_addr);
	fprintf(stderr, "  threads       : %d\n", conf->threads);
	fprintf(stderr, "  max_conns     : %d\n", conf->max_conns);
	fprintf(stderr, "  conn_timeout  : %d\n", conf->conn_timeout);
	fprintf(stderr, "  max_req_size  : %d\n", conf->max_req_size);
	fprintf(stderr, "  mandoc_timeout: %d\n", conf->mandoc_timeout);
	fprintf(stderr, "  static_dir    : %s\n", conf->static_dir);
	fprintf(stderr, "  templates_dir : %s\n", conf->templates_dir);
	fprintf(stderr, "  autoindex     : %d\n", conf->autoindex);
	fprintf(stderr, "  mandoc_path   : %s\n", conf->mandoc_path);
	fprintf(stderr, "  trusted_proxy : %s\n", conf->trusted_proxy);
	fprintf(stderr, "  verbose       : %d\n", conf->verbose);
	fprintf(stderr, "  log_file      : %s\n",
		conf->log_file[0] ? conf->log_file : "(stderr)");
	fprintf(stderr, "  enable_views  : %d\n", conf->enable_views);
	fprintf(stderr, "  enable_metrics: %d\n", conf->enable_metrics);
	fprintf(stderr, "  enable_networking: %d\n", conf->enable_networking);
	fprintf(stderr, "  enable_man    : %d\n", conf->enable_man);
	fprintf(stderr, "  enable_packages: %d\n", conf->enable_packages);
	fprintf(stderr, "=====================================\n");
}
