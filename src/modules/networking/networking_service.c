
#include <miniweb/modules/networking.h>

/** Build networking snapshot JSON from service layer. */
char *
networking_service_build_payload(void)
{
	return networking_get_json();
}
