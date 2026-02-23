#include "../../include/miniweb/storage/sqlite_schema.h"

int
mw_db_migrate(struct mw_db *db,
	const struct mw_migration *migrations, size_t count)
{
	(void)db;
	(void)migrations;
	(void)count;
	return 0;
}

int
mw_tx_begin(struct mw_db *db)
{
	(void)db;
	return 0;
}

int
mw_tx_commit(struct mw_db *db)
{
	(void)db;
	return 0;
}

int
mw_tx_rollback(struct mw_db *db)
{
	(void)db;
	return 0;
}
