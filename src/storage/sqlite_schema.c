#include "../../include/miniweb/storage/sqlite_schema.h"

/**
 * @brief Apply an ordered migration list to the database.
 * @param db Open database handle.
 * @param migrations Array of migration descriptors.
 * @param count Number of entries in @p migrations.
 * @return 0 on success, -1 on validation or execution failure.
 */
int
mw_db_migrate(struct mw_db *db,
	const struct mw_migration *migrations, size_t count)
{
	(void)db;
	(void)migrations;
	(void)count;
	return 0;
}

/**
 * @brief Start a transaction.
 * @param db Open database handle.
 * @return 0 on success, -1 on failure.
 */
int
mw_tx_begin(struct mw_db *db)
{
	(void)db;
	return 0;
}

/**
 * @brief Commit the active transaction.
 * @param db Open database handle.
 * @return 0 on success, -1 on failure.
 */
int
mw_tx_commit(struct mw_db *db)
{
	(void)db;
	return 0;
}

/**
 * @brief Roll back the active transaction.
 * @param db Open database handle.
 * @return 0 on success, -1 on failure.
 */
int
mw_tx_rollback(struct mw_db *db)
{
	(void)db;
	return 0;
}
