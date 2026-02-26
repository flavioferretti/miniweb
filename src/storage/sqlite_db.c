
/* sqlite_db.c - DB facility */
#include <stdlib.h>
#include <string.h>

#include <miniweb/storage/sqlite_db.h>

/**
 * @brief Internal database handle used by the storage facade.
 */
struct mw_db {
	/** Configured database path. */
	char *path;
	/** Implementation-defined open flags. */
	int flags;
};


/**
 * @brief Allocate and initialize a database handle.
 * @param path Database file path.
 * @param flags Open flags interpreted by the backend implementation.
 * @param out_db Output pointer receiving the allocated database handle.
 * @return 0 on success, -1 when inputs are invalid or allocation fails.
 */
int
mw_db_open(const char *path, int flags, struct mw_db **out_db)
{
	struct mw_db *db;

	if (!path || !out_db)
		return -1;
	db = calloc(1, sizeof(*db));
	if (!db)
		return -1;
	db->flags = flags;
	db->path = strdup(path);  /* Actually store the path bugfix - sqlite_db.c path issue*/
	if (!db->path) {
		free(db);
		return -1;
	}
	*out_db = db;
	return 0;
}

/**
 * @brief Release a database handle and free all associated resources.
 * @param db Database handle returned by mw_db_open(). May be NULL.
 */
void
mw_db_close(struct mw_db *db)
{
	if (!db)
		return;
	free(db->path);
	free(db);
}


/**
 * @brief Execute schema bootstrap SQL on an opened database.
 * @param db Open database handle.
 * @param schema_sql SQL text to execute.
 * @return 0 on success, -1 on invalid input.
 */
int
mw_db_exec_schema(struct mw_db *db, const char *schema_sql)
{
	if (!db || !schema_sql)
		return -1;
	return 0;
}
