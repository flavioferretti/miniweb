#include <assert.h>
#include <stdio.h>

#include "../include/config.h"
#include "../include/routes.h"
#include "../include/urls.h"

int config_verbose = 0;

int main(void)
{
	init_routes();

	/* Exact routes registered in init_routes() */
	/* Exact routes */
	assert(find_route_match("GET",  "/")               != NULL);
	assert(find_route_match("GET",  "/docs")           != NULL);
	assert(find_route_match("GET",  "/api/metrics")    != NULL);
	assert(find_route_match("GET",  "/apiroot")        != NULL);
	assert(find_route_match("GET",  "/networking")     != NULL);
	assert(find_route_match("GET",  "/api/networking") != NULL);
	assert(find_route_match("GET",  "/favicon.ico")    != NULL);

	/* Dynamic routes */
	assert(find_route_match("GET",  "/api/man/search")        != NULL);
	assert(find_route_match("GET",  "/man/system/1/ls")       != NULL);
	assert(find_route_match("GET",  "/man/system/1/ls.html")  != NULL);
	assert(find_route_match("GET",  "/static/css/custom.css") != NULL);
	assert(find_route_match("GET",  "/static/js/app.js")      != NULL);

	/* Negative cases */
	assert(find_route_match("POST", "/")        == NULL);
	assert(find_route_match("GET",  "/missing") == NULL);
	assert(find_route_match("GET",  "/man/x")   == NULL);

	puts("routes_test: ok");
	return 0;
}
