/* template_engine.h */

#ifndef TEMPLATE_ENGINE_H
#define TEMPLATE_ENGINE_H

#include <microhttpd.h>

struct template_data {
    const char *title;            /* Page Title (mandatory) */
    const char *page_content;     /* Main content file (mandatory) */
    const char *extra_head_file;  /* File for extra_head (optional) */
    const char *extra_js_file;    /* File for extra_js (optional) */
};

/* Template engine functions */
int template_render_with_data(struct template_data *data, char **output);
int template_render(const char *page, char **output);

#endif
