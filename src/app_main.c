#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <miniweb/core/conf.h>
#include <miniweb/core/config.h>
#include <miniweb/core/log.h>
#include <miniweb/net/server.h>
#include <miniweb/platform/openbsd/security.h>
#include <miniweb/render/template_engine.h>
#include <miniweb/router/routes.h>

static miniweb_conf_t config;
static miniweb_server_runtime_t g_server;

int config_verbose = 0;
char config_static_dir[CONF_STR_MAX] = "static";
char config_templates_dir[CONF_STR_MAX] = "templates";

/** Print command-line usage text. */
static void
usage(const char *prog)
{
	fprintf(stderr,
			"Usage: %s [options]\n"
			"  -f FILE   Config file (default: auto-detect)\n"
			"  -p PORT   Port (default %d)\n"
			"  -b ADDR   Bind address (default %s)\n"
			"  -t NUM    Worker threads (default %d, max %d)\n"
			"  -c NUM    Max connections (default %d)\n"
			"  -l FILE   Log file path (default: stderr)\n"
			"  -v        Verbose\n"
			"  -h        Help\n",
		 prog, config.port, config.bind_addr, config.threads,
		 MINIWEB_THREAD_POOL_SIZE, config.max_conns);
}

/** Parse CLI/config values and propagate global module settings. */
static void
parse_args(int argc, char *argv[])
{
	const char *conf_file = NULL;
	int cli_port = -1, cli_threads = -1, cli_conns = -1, cli_verbose = 0;
	const char *cli_bind = NULL, *cli_log_file = NULL;
	int opt;
	while ((opt = getopt(argc, argv, "f:p:b:t:c:l:vh")) != -1) {
		switch (opt) {
			case 'f': conf_file = optarg; break;
			case 'p': cli_port = atoi(optarg); break;
			case 'b': cli_bind = optarg; break;
			case 't': cli_threads = atoi(optarg); break;
			case 'c': cli_conns = atoi(optarg); break;
			case 'l': cli_log_file = optarg; break;
			case 'v': cli_verbose = 1; break;
			case 'h': usage(argv[0]); exit(0);
			default: usage(argv[0]); exit(1);
		}
	}
	conf_defaults(&config);
	if (conf_load(conf_file, &config) != 0)
		exit(1);
	conf_apply_cli(&config, cli_port, cli_bind, cli_threads, cli_conns,
				   cli_log_file, cli_verbose);
	if (config.threads < 1)
		config.threads = 1;
	if (config.threads > MINIWEB_THREAD_POOL_SIZE)
		config.threads = MINIWEB_THREAD_POOL_SIZE;
	if (config.max_conns > MINIWEB_MAX_CONNECTIONS)
		config.max_conns = MINIWEB_MAX_CONNECTIONS;
	if (config.max_req_size > MINIWEB_REQUEST_BUFFER_SIZE)
		config.max_req_size = MINIWEB_REQUEST_BUFFER_SIZE;
	config_verbose = config.verbose;
	strlcpy(config_static_dir, config.static_dir, sizeof(config_static_dir));
	strlcpy(config_templates_dir, config.templates_dir, sizeof(config_templates_dir));
}

/** Stop the server on signal-driven shutdown requests. */
static void
handle_signal(int sig)
{
	(void)sig;
	miniweb_server_stop(&g_server);
}

/** Start MiniWeb server process and block until shutdown. */
int
main(int argc, char *argv[])
{
	parse_args(argc, argv);
	if (log_init(config.log_file, config.verbose) != 0)
		return 1;
	log_set_verbose(config.verbose);

	log_info("MiniWeb starting on %s:%d (%d thread(s))",
			 config.bind_addr, config.port, config.threads);
	log_info("Static dir: %s  Templates dir: %s",
			 config.static_dir, config.templates_dir);

	if (config.verbose)
		conf_dump(&config);

	if (template_cache_init() != 0) {
		log_error("template_cache_init failed");
		return 1;
	}
	init_routes();
	log_info("Routes registered — listening");

	g_server.config = &config;
	signal(SIGINT, handle_signal);
	signal(SIGTERM, handle_signal);
	miniweb_apply_openbsd_security(&config);
	(void)miniweb_server_run(&g_server);

	log_info("MiniWeb shutting down");
	template_cache_cleanup();
	log_close();
	return 0;
}
