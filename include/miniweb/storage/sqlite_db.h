#ifndef MINIWEB_STORAGE_SQLITE_DB_H
#define MINIWEB_STORAGE_SQLITE_DB_H

#include <stddef.h>

struct mw_db;

int mw_db_open(const char *path, int flags, struct mw_db **out_db);
void mw_db_close(struct mw_db *db);
int mw_db_exec_schema(struct mw_db *db, const char *schema_sql);

#endif
