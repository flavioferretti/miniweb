#include <stdio.h>
#include <string.h>

#include <miniweb/router/urls.h>

#include "url_registry_internal.h"

/** @brief route_allow_methods function. */
int
route_allow_methods(const char *path, char *buf, size_t buf_len)
{
	if (!path || !buf || buf_len == 0)
		return 0;

	buf[0] = '\0';
	size_t used = 0;
	int count = 0;

	for (size_t i = 0; i < route_count; i++) {
		if (strcmp(routes[i].path, path) != 0)
			continue;

		if (route_method_seen_for_path(i, path))
			continue;

		int wrote = snprintf(buf + used, buf_len - used, "%s%s",
			count > 0 ? ", " : "", routes[i].method);
		if (wrote < 0 || (size_t)wrote >= buf_len - used)
			return count;

		used += (size_t)wrote;
		count++;
	}

	for (size_t i = 0; i < prefix_route_count; i++) {
		if (strncmp(path, prefix_routes[i].prefix,
			strlen(prefix_routes[i].prefix)) != 0)
			continue;

		if (count == 0 && buf_len > 3)
			(void)snprintf(buf, buf_len, "%s", prefix_routes[i].method);

		if (count == 0)
			count = 1;
		break;
	}

	return count;
}
