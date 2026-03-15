#include <miniweb/core/conf.h>

/**
 * @brief conf_validate operation.
 *
 * @details Performs the core conf_validate routine for this module.
 *
 * @param conf Input parameter for conf_validate.
 *
 * @return Return value produced by conf_validate.
 */
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
