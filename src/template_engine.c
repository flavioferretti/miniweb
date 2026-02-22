/* template_engine.c - Template Engine Implementation */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>

#include "../include/template_engine.h"
#include "../include/config.h"

/**
 * @brief Internal data structure.
 */
typedef struct template_entry {
	char *filename;
	char *content;
	size_t len;
} template_entry_t;

static template_entry_t *template_cache = NULL;
static size_t template_cache_count = 0;

/**
 * @brief Read an entire file into a newly allocated NUL-terminated buffer.
 * @param path Filesystem path of the file to read.
 * @param content Output pointer that receives the allocated buffer on success.
 * @return Returns 0 on success or -1 on failure.
 */
static int
read_file_content(const char *path, char **content)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return -1;

	/* Determine file size to allocate exact buffer size */
	if (fseek(f, 0, SEEK_END) != 0) {
		fclose(f);
		return -1;
	}
	long file_size = ftell(f);
	if (file_size < 0) {
		fclose(f);
		return -1;
	}
	if (fseek(f, 0, SEEK_SET) != 0) {
		fclose(f);
		return -1;
	}

	/* Allow empty files too. */

	/* Allocate memory; +1 for the null terminator */
	*content = malloc((size_t)file_size + 1);
	if (!*content) {
		fclose(f);
		return -1;
	}

	/* Read file content into the allocated buffer */
	size_t n = fread(*content, 1, (size_t)file_size, f);
	if (n != (size_t)file_size && ferror(f)) {
		free(*content);
		*content = NULL;
		fclose(f);
		return -1;
	}
	(*content)[n] = '\0';
	fclose(f);

	return 0;
}

/**
 * @brief Free all cached templates.
 */
void
template_cache_cleanup(void)
{
	for (size_t i = 0; i < template_cache_count; i++) {
		free(template_cache[i].filename);
		free(template_cache[i].content);
	}
	free(template_cache);
	template_cache = NULL;
	template_cache_count = 0;
}

/**
 * @brief Add one file to the in-memory template cache.
 * @param filename Basename of the template file.
 * @param path Absolute/relative filesystem path to read.
 * @return Returns 0 on success or -1 on failure.
 */
static int
add_template_to_cache(const char *filename, const char *path)
{
	char *content = NULL;
	template_entry_t *new_cache;

	if (read_file_content(path, &content) != 0)
		return -1;

	new_cache = realloc(template_cache,
				   sizeof(*template_cache) * (template_cache_count + 1));
	if (!new_cache) {
		free(content);
		return -1;
	}
	template_cache = new_cache;

	template_cache[template_cache_count].filename = strdup(filename);
	if (!template_cache[template_cache_count].filename) {
		free(content);
		return -1;
	}
	template_cache[template_cache_count].content = content;
	template_cache[template_cache_count].len = strlen(content);
	template_cache_count++;

	return 0;
}

/**
 * @brief Preload template files from configured directory into memory.
 * @return Returns 0 on success or -1 on failure.
 */
int
template_cache_init(void)
{
	DIR *dir;
	struct dirent *entry;
	int loaded = 0;

	template_cache_cleanup();

	dir = opendir(config_templates_dir);
	if (!dir)
		return -1;

	while ((entry = readdir(dir)) != NULL) {
		char path[PATH_MAX];
		struct stat st;
		int n;

		if (strcmp(entry->d_name, ".") == 0 ||
		    strcmp(entry->d_name, "..") == 0)
			continue;

		n = snprintf(path, sizeof(path), "%s/%s", config_templates_dir,
			     entry->d_name);
		if (n < 0 || (size_t)n >= sizeof(path)) {
			closedir(dir);
			template_cache_cleanup();
			return -1;
		}

		if (stat(path, &st) != 0)
			continue;
		if (!S_ISREG(st.st_mode))
			continue;

		if (add_template_to_cache(entry->d_name, path) != 0) {
			closedir(dir);
			template_cache_cleanup();
			return -1;
		}
		loaded = 1;
	}

	closedir(dir);
	return loaded ? 0 : -1;
}

/**
 * @brief Load a template file from the in-memory cache.
 * @param filename Template filename relative to the templates directory.
 * @param content Output pointer that receives the allocated template content.
 * @return Returns 0 on success or -1 on failure.
 */
static int
read_template_file(const char *filename, char **content)
{
	for (size_t i = 0; i < template_cache_count; i++) {
		if (strcmp(template_cache[i].filename, filename) == 0) {
			*content = strdup(template_cache[i].content);
			return *content ? 0 : -1;
		}
	}
	return -1;
}

/* Forward declaration for the single placeholder replacement logic */
static char *replace_single(const char *str, const char *needle,
							const char *value);

/**
 * @brief Replace all supported placeholders in the base template string.
 * @param template_str Base template text containing placeholders.
 * @param title Value for the {{title}} placeholder.
 * @param page_content Value for the {{page_content}} placeholder.
 * @param extra_head Value for the {{extra_head}} placeholder.
 * @param extra_js Value for the {{extra_js}} placeholder.
 * @return Newly allocated rendered string, or NULL on failure.
 */
static char *
replace_all(const char *template_str, const char *title,
			const char *page_content, const char *extra_head,
			const char *extra_js)
{
	char *result = NULL;
	char *temp1 = NULL, *temp2 = NULL, *temp3 = NULL;

	/* Initialize result with a copy of the original template */
	result = strdup(template_str);
	if (!result)
		return NULL;

	/* Replace {{title}} tag */
	temp1 = replace_single(result, "{{title}}", title ? title : "");
	if (!temp1) {
		free(result);
		return NULL;
	}
	free(result);
	result = temp1;

	/* Replace {{page_content}} tag */
	temp2 = replace_single(result, "{{page_content}}",
						   page_content ? page_content : "");
	if (!temp2) {
		free(result);
		return NULL;
	}
	free(result);
	result = temp2;

	/* Replace {{extra_head}} tag */
	temp3 = replace_single(result, "{{extra_head}}",
						   extra_head ? extra_head : "");
	if (!temp3) {
		free(result);
		return NULL;
	}
	free(result);
	result = temp3;

	/* Replace {{extra_js}} tag */
	temp1 =
	replace_single(result, "{{extra_js}}", extra_js ? extra_js : "");
	if (!temp1) {
		free(result);
		return NULL;
	}
	free(result);
	result = temp1;

	return result;
}

/**
 * @brief Replace the first occurrence of a token in a string.
 * @param str Source string to search.
 * @param needle Placeholder token to replace.
 * @param value Replacement value to inject.
 * @return Newly allocated string containing the replacement, or NULL on error.
 */
static char *
replace_single(const char *str, const char *needle, const char *value)
{
	char *pos = strstr(str, needle);
	if (!pos) {
		return strdup(str);
	}

	size_t before_len = (size_t)(pos - str);
	size_t needle_len = strlen(needle);
	size_t value_len = strlen(value);
	size_t after_len = strlen(pos + needle_len);

	/* Allocate exact memory for the new string */
	char *result = malloc(before_len + value_len + after_len + 1);
	if (!result)
		return NULL;

	/* Build the new string from parts: [before][value][after] */
	memcpy(result, str, before_len);
	memcpy(result + before_len, value, value_len);
	memcpy(result + before_len + value_len, pos + needle_len,
	   after_len + 1);

	return result;
}

/**
 * @brief Render a full HTML page using base and content templates.
 * @param data Template metadata and optional fragment filenames.
 * @param output Output pointer that receives the rendered HTML buffer.
 * @return Returns 0 on success or -1 when rendering fails.
 */
int
template_render_with_data(struct template_data *data, char **output)
{
	char *base_template = NULL;
	char *page_content = NULL;
	char *extra_head = NULL;
	char *extra_js = NULL;
	char *result = NULL;
	int ret = -1;

	if (!data || !data->title || !data->page_content) {
		/* Missing mandatory data for rendering */
		return -1;
	}

	/* Load the global base layout (shell) */
	if (read_template_file("base.html", &base_template) != 0) {
		goto cleanup;
	}

	/* Load the specific inner page content */
	if (read_template_file(data->page_content, &page_content) != 0) {
		goto cleanup;
	}

	/* Load optional header file if specified in template_data */
	if (data->extra_head_file) {
		if (read_template_file(data->extra_head_file, &extra_head) !=
			0) {
			/* Fail silently if file is missing, keeping it empty */
			extra_head = NULL;
		}
	}

	/* Load optional JS file if specified in template_data */
	if (data->extra_js_file) {
		if (read_template_file(data->extra_js_file, &extra_js) != 0) {
			extra_js = NULL;
		}
	}

	/* Execute placeholder replacements */
	result =
	replace_all(base_template, data->title, page_content,
			extra_head ? extra_head : "", extra_js ? extra_js : "");
	if (!result) {
		goto cleanup;
	}

	*output = result;
	ret = 0;

cleanup:
	/* Ensure all intermediate buffers are freed */
	if (base_template)
		free(base_template);
	if (page_content)
		free(page_content);
	if (extra_head)
		free(extra_head);
	if (extra_js)
		free(extra_js);

	return ret;
}

/**
 * @brief Backward-compatible wrapper around template_render_with_data().
 */
/**
 * @brief Render a template page with default metadata values.
 * @param page Template filename to load as page content.
 * @param output Output pointer that receives the rendered HTML buffer.
 * @return Returns 0 on success or -1 on failure.
 */
int
template_render(const char *page, char **output)
{
	struct template_data data = {.title = "MiniWeb",
		.page_content = page,
		.extra_head_file = NULL,
		.extra_js_file = NULL};

	return template_render_with_data(&data, output);
}
