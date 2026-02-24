/* sqlite_schema.h - main db schema */
#ifndef MINIWEB_STORAGE_SQLITE_SCHEMA_H
#define MINIWEB_STORAGE_SQLITE_SCHEMA_H

#include <stddef.h>

struct mw_db;

struct mw_migration {
	int version;
	const char *sql;
};

int mw_db_migrate(struct mw_db *db,
	const struct mw_migration *migrations, size_t count);
int mw_tx_begin(struct mw_db *db);
int mw_tx_commit(struct mw_db *db);
int mw_tx_rollback(struct mw_db *db);

#endif
