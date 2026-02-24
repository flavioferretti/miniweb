#include <miniweb/modules/man.h>

/** Run manpage search and return JSON response body. */
char *
man_service_search_json(const char *query)
{
	return man_api_search(query);
}
