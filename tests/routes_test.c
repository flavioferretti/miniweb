#include <assert.h>
#include <stdio.h>
#include "../include/routes.h"

int main(void)
{
	init_routes();
	assert(route_match("GET", "/") != NULL);
	assert(route_match("GET", "/docs") != NULL);
	assert(route_match("GET", "/api/metrics") != NULL);
	assert(route_match("GET", "/api/man") != NULL);
	assert(route_match("GET", "/man/system/1/ls") != NULL);
	assert(route_match("GET", "/static/css/custom.css") != NULL);
	assert(route_match("POST", "/") == NULL);
	assert(route_match("GET", "/missing") == NULL);
	puts("routes_test: ok");
	return 0;
}
