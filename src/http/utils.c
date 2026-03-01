/* utils.c - Utility functions: JSON escaping, string sanitization. */

#include <miniweb/http/utils.h>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

char *
json_escape_string(const char *src)
{
	if (!src)
		return strdup("");

	size_t len = strlen(src);
	size_t max_out = len * 6 + 1;
	char *dest = malloc(max_out);
	if (!dest)
		return strdup("");

	size_t d = 0;
	for (size_t s = 0; src[s] != '\0'; s++) {
		if (d + 7 >= max_out - 1)
			break;

		switch (src[s]) {
		case '"':
			dest[d++] = '\\';
			dest[d++] = '"';
			break;
		case '\\':
			dest[d++] = '\\';
			dest[d++] = '\\';
			break;
		case '\b':
			dest[d++] = '\\';
			dest[d++] = 'b';
			break;
		case '\f':
			dest[d++] = '\\';
			dest[d++] = 'f';
			break;
		case '\n':
			dest[d++] = '\\';
			dest[d++] = 'n';
			break;
		case '\r':
			dest[d++] = '\\';
			dest[d++] = 'r';
			break;
		case '\t':
			dest[d++] = '\\';
			dest[d++] = 't';
			break;
		default:
			if ((unsigned char)src[s] < 0x20)
				d += snprintf(dest + d, max_out - d, "\\u%04x",
					(unsigned char)src[s]);
			else
				dest[d++] = src[s];
			break;
		}
	}
	dest[d] = '\0';
	return dest;
}

void
sanitize_string(char *s)
{
	if (!s)
		return;
	while (*s) {
		if (!isalnum((unsigned char)*s) && *s != '.' && *s != '-' && *s != '_' &&
			*s != '+')
			*s = '_';
		s++;
	}
}

static int
hex_value(char c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return 10 + (c - 'a');
	if (c >= 'A' && c <= 'F')
		return 10 + (c - 'A');
	return -1;
}

int
url_decode(const char *src, char *dst, size_t dst_len)
{
	size_t out = 0;

	if (!src || !dst || dst_len == 0)
		return -1;

	for (size_t i = 0; src[i] != '\0'; i++) {
		if (out + 1 >= dst_len)
			return -1;

		if (src[i] == '%') {
			int hi;
			int lo;
			if (src[i + 1] == '\0' || src[i + 2] == '\0')
				return -1;
			hi = hex_value(src[i + 1]);
			lo = hex_value(src[i + 2]);
			if (hi < 0 || lo < 0)
				return -1;
			dst[out++] = (char)((hi << 4) | lo);
			i += 2;
		} else if (src[i] == '+') {
			dst[out++] = ' ';
		} else {
			dst[out++] = src[i];
		}
	}

	dst[out] = '\0';
	return 0;
}

const char *
mime_type_for_path(const char *path)
{
	const char *ext;

	if (!path)
		return "application/octet-stream";

	ext = strrchr(path, '.');
	if (!ext)
		return "application/octet-stream";

	if (strcmp(ext, ".html") == 0)
		return "text/html";
	if (strcmp(ext, ".css") == 0)
		return "text/css";
	if (strcmp(ext, ".js") == 0)
		return "application/javascript";
	if (strcmp(ext, ".png") == 0)
		return "image/png";
	if (strcmp(ext, ".svg") == 0)
		return "image/svg+xml";
	if (strcmp(ext, ".jpg") == 0 || strcmp(ext, ".jpeg") == 0)
		return "image/jpeg";
	if (strcmp(ext, ".gif") == 0)
		return "image/gif";
	if (strcmp(ext, ".ico") == 0)
		return "image/x-icon";
	if (strcmp(ext, ".pdf") == 0)
		return "application/pdf";
	if (strcmp(ext, ".ps") == 0)
		return "application/postscript";
	if (strcmp(ext, ".md") == 0)
		return "text/markdown; charset=utf-8";
	if (strcmp(ext, ".txt") == 0)
		return "text/plain; charset=utf-8";

	return "application/octet-stream";
}
