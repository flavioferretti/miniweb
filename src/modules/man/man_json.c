#include <miniweb/modules/man.h>

/** Serialize manual section catalog into JSON. */
char *
man_json_sections(void)
{
	return man_get_sections_json();
}
