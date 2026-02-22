#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/template_engine.h"

char config_templates_dir[] = "templates";

/**
 * @brief Validate template rendering with baseline template data.
 * @return Returns 0 when all assertions pass.
 */
int main(void)
{
	char *out = NULL;
	struct template_data data = {
		.title = "MiniWeb - Test",
		.page_content = "api.html",
		.extra_head_file = NULL,
		.extra_js_file = NULL,
	};
	assert(template_cache_init() == 0);
	assert(template_render_with_data(&data, &out) == 0);
	assert(out != NULL);
	assert(strstr(out, "MiniWeb - Test") != NULL);
	assert(strstr(out, "<html") != NULL);
	free(out);
	template_cache_cleanup();
	puts("template_test: ok");
	return 0;
}
