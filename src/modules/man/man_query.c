#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "man_internal.h"

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

int
man_is_valid_section(const char *section)
{
    if (!section || *section == '\0' || strlen(section) > 8)
        return 0;
    for (const unsigned char *p = (const unsigned char *)section; *p; p++) {
        if (!isalnum(*p))
            return 0;
    }
    return 1;
}

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

const char *
man_area_from_path(const char *filepath)
{
    if (strncmp(filepath, "/usr/X11R6/", 11) == 0)
        return "x11";
    if (strncmp(filepath, "/usr/local/", 11) == 0)
        return "packages";
    return "system";
}
