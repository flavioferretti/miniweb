#include <assert.h>
#include <stdio.h>
#include <string.h>

#include <miniweb/core/config.h>
#include <miniweb/core/conf.h>
#include <miniweb/router/routes.h>
#include <miniweb/router/urls.h>

int config_verbose = 0;
char config_static_dir[] = "static";
char config_templates_dir[] = "templates";
int config_autoindex = 0;
miniweb_conf_t config = {0};

/**
 * @brief TODO: Describe main.
 * @return TODO: Describe the return value.
 */
/**
 * @brief Validate route registration and route matching behavior.
 * @return Returns 0 when all assertions pass.
 */
int main(void)
{
	init_routes(NULL);

	/* Exact routes registered in init_routes() */
	assert(route_match("GET",  "/")               != NULL);
	assert(route_match("GET",  "/docs")           != NULL);
	assert(route_match("GET",  "/api/metrics")    != NULL);
	assert(route_match("GET",  "/apiroot")        != NULL);
	assert(route_match("GET",  "/networking")     != NULL);
	assert(route_match("GET",  "/api/networking") != NULL);
	assert(route_match("GET",  "/packages")       != NULL);
	assert(route_match("GET",  "/api/packages/search") != NULL);
	assert(route_match("GET",  "/api/packages/info")   != NULL);
	assert(route_match("GET",  "/api/packages/which")  != NULL);
	assert(route_match("GET",  "/api/packages/files")  != NULL);
	assert(route_match("GET",  "/api/packages/list")   != NULL);
	assert(route_match("GET",  "/favicon.ico")    != NULL);

	/* Dynamic routes */
	assert(route_match("GET",  "/api/man/search")        != NULL);
	assert(route_match("GET",  "/man/system/1/ls")       != NULL);
	assert(route_match("GET",  "/man/system/1/ls.html")  != NULL);
	assert(route_match("GET",  "/static/css/custom.css") != NULL);
	assert(route_match("GET",  "/static/js/app.js")      != NULL);

	/* Negative cases */
	assert(route_match("POST", "/")        == NULL);
	char allow[64];
	assert(route_allow_methods("/", allow, sizeof(allow)) == 1);
	assert(strcmp(allow, "GET") == 0);
	assert(route_path_known("/")             == 1);
	assert(route_path_known("/api/man/search") == 1);
	assert(route_path_known("/static/css/custom.css") == 1);
	assert(route_path_known("/missing")      == 0);
	assert(route_match("GET",  "/missing") == NULL);
	assert(route_match("GET",  "/man/x")   == NULL);

	/* Module toggle: disable views only */
	memset(&config, 0, sizeof(config));
	config.enable_views = 0;
	config.enable_metrics = 1;
	config.enable_networking = 1;
	config.enable_man = 1;
	config.enable_packages = 1;
	init_routes(&config);
	assert(route_match("GET", "/") == NULL);
	assert(route_match("GET", "/docs") == NULL);
	assert(route_match("GET", "/api/metrics") != NULL);

	puts("routes_test: ok");
	return 0;
}
