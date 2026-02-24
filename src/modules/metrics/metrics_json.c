#include <miniweb/modules/metrics.h>

/** Compatibility adapter for JSON generation delegation. */
char *
metrics_json_build(void)
{
	return get_system_metrics_json();
}
