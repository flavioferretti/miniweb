#include <miniweb/core/conf.h>

/** @brief conf_validate function. */
int
conf_validate(miniweb_conf_t *conf)
{
	if (conf->port <= 0 || conf->port > 65535)
		return -1;
	if (conf->threads <= 0)
		return -1;
	if (conf->max_conns <= 0)
		return -1;
	if (conf->conn_timeout <= 0)
		return -1;
	if (conf->max_req_size <= 0)
		return -1;
	if (conf->mandoc_timeout <= 0)
		return -1;
	return 0;
}
