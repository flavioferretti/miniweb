#include <assert.h>
#include <stdio.h>

#include "../include/config.h"
#include "../include/routes.h"
#include "../include/urls.h"

int config_verbose = 0;
char config_static_dir[] = "static";
char config_templates_dir[] = "templates";

int main(void)
{
	init_routes();

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
	assert(route_match("GET",  "/favicon.ico")    != NULL);

	/* Dynamic routes */
	assert(route_match("GET",  "/api/man/search")        != NULL);
	assert(route_match("GET",  "/man/system/1/ls")       != NULL);
	assert(route_match("GET",  "/man/system/1/ls.html")  != NULL);
	assert(route_match("GET",  "/static/css/custom.css") != NULL);
	assert(route_match("GET",  "/static/js/app.js")      != NULL);

	/* Negative cases */
	assert(route_match("POST", "/")        == NULL);
	assert(route_match("GET",  "/missing") == NULL);
	assert(route_match("GET",  "/man/x")   == NULL);

	puts("routes_test: ok");
	return 0;
}
