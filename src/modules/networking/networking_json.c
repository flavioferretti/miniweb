#include <miniweb/modules/networking.h>

/** Compatibility JSON facade for networking payload creation. */
char *
networking_json_build(void)
{
	return networking_get_json();
}
