#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../include/template_engine.h"

int main(void)
{
	char *out = NULL;
	struct template_data data = {
		.title = "MiniWeb - Test",
		.page_content = "api.html",
		.extra_head_file = NULL,
		.extra_js_file = NULL,
	};
	assert(template_render_with_data(&data, &out) == 0);
	assert(out != NULL);
	assert(strstr(out, "MiniWeb - Test") != NULL);
	assert(strstr(out, "<html") != NULL);
	free(out);
	puts("template_test: ok");
	return 0;
}
