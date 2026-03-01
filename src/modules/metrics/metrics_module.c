/* metrics_module.c - Metrics API routing and endpoint orchestration */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <miniweb/http/handler.h>
#include <miniweb/http/utils.h>
#include <miniweb/modules/metrics.h>
#include <miniweb/router/router.h>

/**
 * @brief Handle GET /api/metrics by returning the latest metrics snapshot.
 * @param req Request context used by the HTTP layer.
 * @return HTTP send status code from the response layer.
 */
int
metrics_handler(http_request_t *req)
{
	char *json = get_system_metrics_json();
	if (!json)
		return http_send_error(req, 500, "Unable to generate metrics");

	http_response_t *resp = http_response_create();
	if (!resp) {
		free(json);
		return http_send_error(req, 500, "Unable to allocate response");
	}

	resp->status_code = 200;
	resp->content_type = "application/json";

	/* Allow access from external dashboards. */
	http_response_add_header(resp, "Access-Control-Allow-Origin", "*");
	http_response_add_header(resp, "Cache-Control", "no-store");

	/* Attach JSON as response body.
	 * The '1' flag tells the response layer to free memory automatically. */
	http_response_set_body(resp, json, strlen(json), 1);

	int ret = http_response_send(req, resp);
	http_response_free(resp);
	return ret;
}

/**
 * @brief Attach metrics API routes to the router.
 * @param r Router to receive route registrations.
 * @return 0 on success, -1 on registration failure.
 */
int
metrics_module_attach_routes(struct router *r)
{
	if (router_register(r, "GET", "/api/metrics", metrics_handler) != 0)
		return -1;

	/* Compatibility alias for clients that call singular form. */
	return router_register(r, "GET", "/api/metric", metrics_handler);
}
