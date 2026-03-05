
#include <miniweb/modules/man.h>

/**
 * @brief Serialize the static manual section catalog into JSON.
 *
 * @details Delegates to `man_get_sections_json()` and returns an allocated
 * JSON string describing area and section metadata.
 *
 * @return char* Heap-allocated JSON document, or NULL on allocation failure.
 */
char *
man_json_sections(void)
{
	return man_get_sections_json();
}
