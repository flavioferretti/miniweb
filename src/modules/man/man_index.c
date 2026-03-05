#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <miniweb/http/utils.h>
#include <miniweb/modules/man.h>
#include "man_internal.h"

/**
 * @brief qsort comparator for arrays of C-string pointers.
 *
 * @param a Pointer to first `char*` element.
 * @param b Pointer to second `char*` element.
 *
 * @return int Negative/zero/positive according to lexical order.
 */
static int
compare_string_ptrs(const void *a, const void *b)
{
    const char *const *left = a;
    const char *const *right = b;
    return strcmp(*left, *right);
}

/**
 * @brief Select a usable path from `man -w` raw output.
 *
 * @param raw_output Heap buffer containing command output; consumed by this
 * function.
 *
 * @return char* Newly allocated absolute path, or NULL when no valid path is
 * found.
 */
static char *
select_resolved_man_path(char *raw_output)
{
    char *selected = NULL;
    if (!raw_output)
        return NULL;

    for (char *p = raw_output; *p; p++) {
        if (*p == '\n' || *p == '\r' || *p == '\t')
            *p = ' ';
    }

    char *saveptr = NULL;
    for (char *tok = strtok_r(raw_output, " ", &saveptr); tok;
         tok = strtok_r(NULL, " ", &saveptr)) {
        if (tok[0] != '/')
            continue;
        if (!selected)
            selected = tok;
        if (access(tok, R_OK) == 0) {
            selected = tok;
            break;
        }
    }

    if (!selected || selected[0] == '\0') {
        free(raw_output);
        return NULL;
    }

    char *resolved = strdup(selected);
    free(raw_output);
    return resolved;
}

/**
 * @brief Resolve a man page name/section to an absolute file path.
 *
 * @param name Manual page name (e.g. `ls`).
 * @param section Section token (e.g. `1`, `3p`).
 *
 * @return char* Heap-allocated absolute path, or NULL when unresolved.
 */
char *
man_resolve_path(const char *name, const char *section)
{
    if (!man_is_valid_token(name) || !man_is_valid_section(section))
        return NULL;

    char *const argv[] = {
        "man",
        "-M", "/usr/share/man:/usr/local/man:/usr/X11R6/man",
        "-w",
        (char *)section,
        (char *)name,
        NULL
    };
    char *raw = safe_popen_read_argv("/usr/bin/man", argv, 2048, 5, NULL);
    return select_resolved_man_path(raw);
}

/**
 * @brief Return JSON describing supported manual areas and sections.
 *
 * @details Produces a static catalog for system, X11, and package man trees.
 *
 * @return char* Heap-allocated JSON document.
 */
char *
man_get_sections_json(void)
{
    return strdup(
        "{\"system\":{"
        "\"name\":\"OpenBSD Base System\"," 
        "\"path\":\"/usr/share/man\","
        "\"sections\":["
        "{\"id\":\"1\",\"name\":\"General Commands\"},"
        "{\"id\":\"2\",\"name\":\"System Calls\"},"
        "{\"id\":\"3\",\"name\":\"Library Functions\"},"
        "{\"id\":\"3p\",\"name\":\"Perl Library\"},"
        "{\"id\":\"4\",\"name\":\"Device Drivers\"},"
        "{\"id\":\"5\",\"name\":\"File Formats\"},"
        "{\"id\":\"6\",\"name\":\"Games\"},"
        "{\"id\":\"7\",\"name\":\"Miscellaneous\"},"
        "{\"id\":\"8\",\"name\":\"System Administration\"},"
        "{\"id\":\"9\",\"name\":\"Kernel Internals\"}"
        "]},"
        "\"x11\":{"
        "\"name\":\"X11 Window System\","
        "\"path\":\"/usr/X11R6/man\","
        "\"sections\":["
        "{\"id\":\"1\",\"name\":\"X11 Commands\"},"
        "{\"id\":\"3\",\"name\":\"X11 Library\"},"
        "{\"id\":\"4\",\"name\":\"X11 Drivers\"},"
        "{\"id\":\"5\",\"name\":\"X11 Formats\"},"
        "{\"id\":\"7\",\"name\":\"X11 Misc\"}"
        "]},"
        "\"packages\":{"
        "\"name\":\"Local Packages\","
        "\"path\":\"/usr/local/man\","
        "\"sections\":["
        "{\"id\":\"1\",\"name\":\"Pkg General\"},"
        "{\"id\":\"2\",\"name\":\"Pkg Calls\"},"
        "{\"id\":\"3\",\"name\":\"Pkg Lib\"},"
        "{\"id\":\"3p\",\"name\":\"Pkg Perl\"},"
        "{\"id\":\"4\",\"name\":\"Pkg Drivers\"},"
        "{\"id\":\"5\",\"name\":\"Pkg Formats\"},"
        "{\"id\":\"6\",\"name\":\"Pkg Games\"},"
        "{\"id\":\"7\",\"name\":\"Pkg Misc\"},"
        "{\"id\":\"8\",\"name\":\"Pkg Admin\"},"
        "{\"id\":\"9\",\"name\":\"Pkg Kernel\"}"
        "]}}");
}

/**
 * @brief Enumerate all pages within a specific area/section as JSON.
 *
 * @param area Area identifier (`system`, `x11`, or `packages`).
 * @param section Section identifier to filter file names.
 *
 * @return char* Heap-allocated JSON object with a `pages` array.
 */
char *
man_get_section_pages_json(const char *area, const char *section)
{
    char dir_path[256];
    char *pages[8192];
    size_t page_count = 0;
    const char *base = "/usr/share/man";

    if (strcmp(area, "packages") == 0)
        base = "/usr/local/man";
    else if (strcmp(area, "x11") == 0)
        base = "/usr/X11R6/man";

    snprintf(dir_path, sizeof(dir_path), "%s/man%s", base, section);

    DIR *dr = opendir(dir_path);
    if (!dr)
        return strdup("{\"pages\":[]}");

    char *json = malloc(MAN_MAX_JSON_SIZE);
    if (!json) {
        closedir(dr);
        return NULL;
    }

    #define JSON_CLOSE_RESERVE 4
    int n = snprintf(json, MAN_MAX_JSON_SIZE, "{\"pages\":[");
    size_t used = (n > 0) ? (size_t)n : 0;

    struct dirent *de;
    int first = 1;

    while ((de = readdir(dr)) != NULL) {
        if (de->d_name[0] == '.')
            continue;
        if (de->d_type == DT_DIR)
            continue;

        char resolved_section[16];
        if (!man_parse_section_from_filename(de->d_name, resolved_section,
            sizeof(resolved_section)))
            continue;
        if (strcmp(resolved_section, section) != 0)
            continue;

        // Extract the page name by removing the .section suffix
        // But keep any other dots in the name (e.g., cupsd.conf.5 -> cupsd.conf)
        char name[128];
        strlcpy(name, de->d_name, sizeof(name));

        // Find the last dot (which is the section separator)
        char *last_dot = strrchr(name, '.');
        if (last_dot) {
            *last_dot = '\0';  // Remove the section suffix
        }

        if (page_count < (sizeof(pages) / sizeof(pages[0]))) {
            pages[page_count] = strdup(name);
            if (pages[page_count])
                page_count++;
        }
    }
    closedir(dr);

    if (page_count > 1)
        qsort(pages, page_count, sizeof(pages[0]), compare_string_ptrs);

    for (size_t i = 0; i < page_count; i++) {
        size_t name_len = strlen(pages[i]);
        size_t entry_len = (first ? 0 : 1) + 1 + name_len + 1;
        if (used + entry_len + JSON_CLOSE_RESERVE >= MAN_MAX_JSON_SIZE) {
            free(pages[i]);
            continue;
        }
        n = snprintf(json + used, MAN_MAX_JSON_SIZE - used - JSON_CLOSE_RESERVE,
                     "%s\"%s\"", first ? "" : ",", pages[i]);
        free(pages[i]);
        if (n > 0) {
            used += (size_t)n;
            first = 0;
        }
    }
    (void)snprintf(json + used, MAN_MAX_JSON_SIZE - used, "]}");
    #undef JSON_CLOSE_RESERVE

    return json;
}

/**
 * @brief Build metadata JSON for one manual page lookup.
 *
 * @param area Area name attached to the response payload.
 * @param section Requested section.
 * @param name Manual page name.
 *
 * @return char* Heap-allocated metadata JSON, or an error JSON string.
 */
char *
man_get_page_metadata_json(const char *area, const char *section, const char *name)
{
    char *filepath = man_resolve_path(name, section);
    if (!filepath)
        return strdup("{\"error\":\"Not found\"}");
    char *json = malloc(1024);
    if (!json) {
        free(filepath);
        return strdup("{\"error\":\"OOM\"}");
    }
    snprintf(json, 1024,
             "{\"name\":\"%s\",\"section\":\"%s\",\"area\":\"%s\","
             "\"path\":\"%s\"}",
             name, section, area, filepath);
    free(filepath);
    return json;
}

/**
 * @brief Run apropos search for a validated query token.
 *
 * @param query Search term.
 *
 * @return char* Heap-allocated raw apropos output; empty string on failure.
 */
char *
man_api_search(const char *query)
{
    if (!man_is_valid_token(query))
        return strdup("");
    char *const argv[] = {
        "apropos",
        "-M", "/usr/share/man:/usr/local/man:/usr/X11R6/man",
        (char *)query, NULL
    };
    char *output = safe_popen_read_argv("/usr/bin/apropos", argv,
                                        MAN_MAX_OUTPUT_SIZE, 5, NULL);
    if (!output)
        return strdup("");
    return output;
}

/**
 * @brief Run apropos search and provide minimal fallback output.
 *
 * @param query Search term from API clients.
 *
 * @return char* Heap-allocated text body. Empty string when no results and no
 * fallback page can be resolved.
 */
char *
man_api_search_raw(const char *query)
{
    if (!query || strlen(query) < 2 || !man_is_valid_token(query))
        return strdup("");

    char *const argv[] = {
        "apropos",
        "-M", "/usr/share/man:/usr/local/man:/usr/X11R6/man",
        (char *)query, NULL
    };
    char *output = safe_popen_read_argv("/usr/bin/apropos", argv,
                                        1024 * 1024, 5, NULL);
    if (!output)
        output = strdup("");

    if (output[0] == '\0') {
        char *filepath = man_resolve_path(query, "1");
        if (!filepath)
            filepath = man_resolve_path(query, "8");

        if (filepath && filepath[0] != '\0') {
            char section[16] = {0};
            const char *base = strrchr(filepath, '/');
            base = base ? base + 1 : filepath;
            if (!man_parse_section_from_filename(base, section, sizeof(section)))
                strlcpy(section, "?", sizeof(section));
            char line[256];
            snprintf(line, sizeof(line), "%s (%s) - manual page", query, section);
            free(output);
            output = strdup(line);
            if (!output)
                output = strdup("");
        }
        free(filepath);
    }
    return output;
}
