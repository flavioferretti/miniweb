#include <miniweb/modules/pkg_manager.h>

/** Build package search payload via the service layer abstraction. */
char *
packages_service_search_json(const char *query)
{
	return pkg_search_json(query);
}
