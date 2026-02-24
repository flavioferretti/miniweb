#include <miniweb/modules/metrics.h>

/** Collect and return the canonical metrics JSON payload. */
char *
metrics_service_build_payload(void)
{
	return get_system_metrics_json();
}
