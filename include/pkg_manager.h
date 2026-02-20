#ifndef PKG_MANAGER_H
#define PKG_MANAGER_H

#include "http_handler.h"

char *pkg_search_json(const char *query);
char *pkg_info_json(const char *package_name);
char *pkg_which_json(const char *file_path);
char *pkg_files_json(const char *package_name);
char *pkg_list_json(void);

int pkg_api_handler(http_request_t *req);

#endif /* PKG_MANAGER_H */
