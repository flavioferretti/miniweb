#include <miniweb/http/handler.h>

#include <arpa/inet.h>
#include <miniweb/core/config.h>
#include <miniweb/core/conf.h>
#include <miniweb/router/urls.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern miniweb_conf_t config;

const char *
http_request_get_header(http_request_t *req, const char *name)
{
	const char *colon;
	const char *end;
	const char *header;
	const char *val;
	char search[256];
	size_t len;

	snprintf(search, sizeof(search), "\r\n%s:", name);
	header = strcasestr(req->buffer, search);
	if (!header) {
		snprintf(search, sizeof(search), "%s:", name);
		header = strcasestr(req->buffer, search);
		if (!header)
			return NULL;
	}

	colon = strchr(header, ':');
	if (!colon)
		return NULL;
	val = colon + 1;
	while (*val == ' ' || *val == '\t')
		val++;

	end = strstr(val, "\r\n");
	if (!end)
		return NULL;
	len = (size_t)(end - val);
	if (len >= sizeof(req->hdr_scratch))
		len = sizeof(req->hdr_scratch) - 1;

	memcpy(req->hdr_scratch, val, len);
	req->hdr_scratch[len] = '\0';
	return req->hdr_scratch;
}

const char *
http_request_get_client_ip(http_request_t *req)
{
	char peer_ip[INET_ADDRSTRLEN] = {0};
	const char *comma;
	const char *forwarded;
	const char *real_ip;
	size_t len;

	if (req->client_addr) {
		inet_ntop(AF_INET, &req->client_addr->sin_addr, peer_ip,
		    sizeof(peer_ip));
	}

	if (config.trusted_proxy[0] != '\0' &&
	    strcmp(peer_ip, config.trusted_proxy) == 0) {
		real_ip = http_request_get_header(req, "X-Real-IP");
		if (real_ip && real_ip[0]) {
			strlcpy(req->ip_scratch, real_ip, sizeof(req->ip_scratch));
			return req->ip_scratch;
		}

		forwarded = http_request_get_header(req, "X-Forwarded-For");
		if (forwarded && forwarded[0]) {
			comma = strchr(forwarded, ',');
			len = comma ? (size_t)(comma - forwarded) : strlen(forwarded);
			if (len >= sizeof(req->ip_scratch))
				len = sizeof(req->ip_scratch) - 1;
			memcpy(req->ip_scratch, forwarded, len);
			req->ip_scratch[len] = '\0';
			return req->ip_scratch;
		}
	}

	strlcpy(req->ip_scratch, peer_ip, sizeof(req->ip_scratch));
	return req->ip_scratch;
}

int
http_request_is_https(http_request_t *req)
{
	char peer_ip[INET_ADDRSTRLEN] = {0};
	const char *proto;

	if (req->client_addr) {
		inet_ntop(AF_INET, &req->client_addr->sin_addr, peer_ip,
		    sizeof(peer_ip));
	}
	if (!(config.trusted_proxy[0] != '\0' &&
	    strcmp(peer_ip, config.trusted_proxy) == 0))
		return 0;

	proto = http_request_get_header(req, "X-Forwarded-Proto");
	return proto && strcmp(proto, "https") == 0;
}

int
http_send_error(http_request_t *req, int status_code, const char *message)
{
	char allow[256];
	char body[2048];
	int body_len;
	int ret;
	http_response_t *resp;

	body_len = snprintf(body, sizeof(body),
	    "<!DOCTYPE html><html><head>"
	    "<meta charset=\"UTF-8\">"
	    "<title>%d Error</title>"
	    "<link rel=\"stylesheet\" href=\"/static/css/custom.css\">"
	    "</head><body>"
	    "<div class=\"container\">"
	    "<h1>%d Error</h1>"
	    "<p>%s</p>"
	    "<hr><p><a href=\"/\">MiniWeb</a> on OpenBSD</p></div>"
	    "</body></html>",
	    status_code, status_code, message ? message : "An error occurred");

	resp = http_response_create();
	if (!resp)
		return -1;
	http_response_set_status(resp, status_code);
	http_response_set_body(resp, body, (size_t)body_len, 0);

	if (status_code == 405 && req && req->url) {
		if (route_allow_methods(req->url, allow, sizeof(allow)) > 0)
			http_response_add_header(resp, "Allow", allow);
	}

	ret = http_response_send(req, resp);
	http_response_free(resp);
	return ret;
}

int
http_send_json(http_request_t *req, const char *json)
{
	http_response_t *resp;
	int ret;

	resp = http_response_create();
	if (!resp)
		return -1;
	resp->content_type = "application/json";
	http_response_set_body(resp, (char *)json, strlen(json), 0);
	ret = http_response_send(req, resp);
	http_response_free(resp);
	return ret;
}

int
http_send_html(http_request_t *req, const char *html)
{
	http_response_t *resp;
	int ret;

	resp = http_response_create();
	if (!resp)
		return -1;
	http_response_set_body(resp, (char *)html, strlen(html), 0);
	ret = http_response_send(req, resp);
	http_response_free(resp);
	return ret;
}

int
http_render_template(http_request_t *req, struct template_data *data,
    const char *fallback_template)
{
	char *output;
	http_response_t *resp;
	int ret;

	output = NULL;
	if (template_render_with_data(data, &output) != 0) {
		if (data->page_content && template_render(data->page_content, &output) !=
		    0) {
			return http_send_error(req, 500, fallback_template ?
			    fallback_template : "Template rendering failed");
		}
	}

	resp = http_response_create();
	if (!resp) {
		free(output);
		return -1;
	}
	http_response_set_body(resp, output, strlen(output), 1);
	ret = http_response_send(req, resp);
	http_response_free(resp);
	return ret;
}
