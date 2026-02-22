/* template_engine.h */

#ifndef TEMPLATE_ENGINE_H
#define TEMPLATE_ENGINE_H

struct template_data {
	const char *title;	     /* Page title (mandatory) */
	const char *page_content;    /* Main content file (mandatory) */
	const char *extra_head_file; /* Additional <head> fragment (optional) */
	const char *extra_js_file;   /* Additional JS fragment (optional) */
};

/**
 * Render the base template using structured template inputs.
 *
 * @param data Input data that selects template fragments.
 * @param output Output pointer that receives a heap-allocated HTML string.
 * @return 0 on success, -1 on failure.
 */
int template_render_with_data(struct template_data *data, char **output);

/**
 * Convenience wrapper that renders a single page by name.
 *
 * @param page Page template filename.
 * @param output Output pointer that receives a heap-allocated HTML string.
 * @return 0 on success, -1 on failure.
 */
int template_render(const char *page, char **output);

/**
 * Preload all regular files from the templates directory into memory.
 *
 * @return 0 on success, -1 on failure.
 */
int template_cache_init(void);

/**
 * Free all in-memory template cache entries.
 */
void template_cache_cleanup(void);

#endif
