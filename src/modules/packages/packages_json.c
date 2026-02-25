
#include <miniweb/modules/pkg_manager.h>

/** Serialize package list payload for API consumers. */
char *
packages_json_list(void)
{
	return pkg_list_json();
}
