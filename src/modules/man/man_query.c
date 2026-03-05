#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "man_internal.h"

/**
 * @brief Check whether a URL path matches an API endpoint literal.
 *
 * @param path Request path (possibly including query string).
 * @param endpoint Endpoint prefix to compare against.
 *
 * @return int Returns 1 when the path matches exactly (or is followed by `?`),
 * otherwise 0.
 */
int
man_path_matches_endpoint(const char *path, const char *endpoint)
{
    size_t len;
    if (!path || !endpoint)
        return 0;
    len = strlen(endpoint);
    if (strncmp(path, endpoint, len) != 0)
        return 0;
    return path[len] == '\0' || path[len] == '?';
}

/**
 * @brief URL-decode a source buffer into a destination buffer.
 *
 * @param src Percent-encoded input string.
 * @param dst Output buffer receiving decoded text.
 * @param dst_size Capacity of @p dst in bytes.
 *
 * @return int Returns 0 on success, -1 on invalid arguments or overflow.
 */
static int
url_decode_into(const char *src, char *dst, size_t dst_size)
{
    size_t di = 0;
    if (!src || !dst || dst_size == 0)
        return -1;
    for (size_t si = 0; src[si] != '\0'; si++) {
        if (di + 1 >= dst_size)
            return -1;
        if (src[si] == '+') {
            dst[di++] = ' ';
            continue;
        }
        if (src[si] == '%' && isxdigit((unsigned char)src[si + 1]) &&
            isxdigit((unsigned char)src[si + 2])) {
            char hex[3] = {src[si + 1], src[si + 2], '\0'};
            dst[di++] = (char)strtol(hex, NULL, 16);
            si += 2;
            continue;
        }
        dst[di++] = src[si];
    }
    dst[di] = '\0';
    return 0;
}

/**
 * @brief Extract and decode a query parameter from a URL.
 *
 * @param url Full URL containing an optional query string.
 * @param key Parameter name to find.
 * @param out Output buffer receiving decoded parameter value.
 * @param out_size Capacity of @p out in bytes.
 *
 * @return int Returns 1 when the key is present and decoded, otherwise 0.
 */
int
man_get_query_value(const char *url, const char *key, char *out, size_t out_size)
{
    const char *qs;
    size_t key_len;

    if (!url || !key || !out || out_size == 0)
        return 0;
    qs = strchr(url, '?');
    if (!qs)
        return 0;
    qs++;
    key_len = strlen(key);

    while (*qs) {
        const char *entry = qs;
        const char *eq = strchr(entry, '=');
        const char *amp = strchr(entry, '&');
        if (!amp)
            amp = entry + strlen(entry);
        if (!eq || eq > amp) {
            qs = (*amp == '&') ? amp + 1 : amp;
            continue;
        }
        if ((size_t)(eq - entry) == key_len && strncmp(entry, key, key_len) == 0) {
            char encoded[512];
            size_t encoded_len = (size_t)(amp - (eq + 1));
            if (encoded_len >= sizeof(encoded))
                return 0;
            memcpy(encoded, eq + 1, encoded_len);
            encoded[encoded_len] = '\0';
            return url_decode_into(encoded, out, out_size) == 0;
        }
        qs = (*amp == '&') ? amp + 1 : amp;
    }
    return 0;
}

/**
 * @brief Validate user-provided manual page tokens.
 *
 * @param s Token string such as command/page name.
 *
 * @return int Returns 1 for valid tokens, 0 for invalid or empty tokens.
 */
int
man_is_valid_token(const char *s)
{
    if (!s || *s == '\0')
        return 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        if (!isalnum(*p) && *p != '.' && *p != '_' && *p != '-' && *p != '+')
            return 0;
    }
    return 1;
}

/**
 * @brief Validate a manual section identifier.
 *
 * @param section Section string (e.g. `1`, `3p`, `8`).
 *
 * @return int Returns 1 for valid section syntax, otherwise 0.
 */
int
man_is_valid_section(const char *section)
{
    if (!section || *section == '\0' || strlen(section) > 8)
        return 0;

    // OpenBSD man sections are numbers, optionally with a letter suffix
    // Valid examples: "1", "2", "3", "3p", "4", "5", "6", "7", "8", "9"
    for (const unsigned char *p = (const unsigned char *)section; *p; p++) {
        // First character must be a digit
        if (p == (const unsigned char *)section) {
            if (!isdigit(*p))
                return 0;
        } else {
            // Subsequent characters can only be lowercase letters
            if (!islower(*p))
                return 0;
        }
    }
    return 1;
}

/**
 * @brief Parse section suffix from a man filename.
 *
 * @param filename Basename such as `ls.1`, optionally compressed.
 * @param section_out Output buffer receiving parsed section token.
 * @param section_out_len Capacity of @p section_out in bytes.
 *
 * @return int Returns 1 when a valid section is found, else 0.
 */
int
man_parse_section_from_filename(const char *filename, char *section_out,
                                size_t section_out_len)
{
    static const char *compressed_suffixes[] = {".gz", ".bz2", ".xz", ".zst"};
    char tmp[256];

    if (!filename || !section_out || section_out_len == 0)
        return 0;
    strlcpy(tmp, filename, sizeof(tmp));

    for (size_t i = 0; i < sizeof(compressed_suffixes) / sizeof(compressed_suffixes[0]); i++) {
        size_t tlen = strlen(tmp);
        size_t slen = strlen(compressed_suffixes[i]);
        if (tlen > slen && strcmp(tmp + tlen - slen, compressed_suffixes[i]) == 0) {
            tmp[tlen - slen] = '\0';
            break;
        }
    }

    char *dot = strrchr(tmp, '.');
    if (!dot || dot[1] == '\0' || !man_is_valid_section(dot + 1))
        return 0;
    strlcpy(section_out, dot + 1, section_out_len);
    return 1;
}

/**
 * @brief Infer logical man area from an absolute file path.
 *
 * @param filepath Absolute path to a resolved manual page file.
 *
 * @return const char* Area identifier string: `x11`, `packages`, or `system`.
 */
const char *
man_area_from_path(const char *filepath)
{
    if (strncmp(filepath, "/usr/X11R6/", 11) == 0)
        return "x11";
    if (strncmp(filepath, "/usr/local/", 11) == 0)
        return "packages";
    return "system";
}
