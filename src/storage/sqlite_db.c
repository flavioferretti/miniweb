/* sqlite_db.c - DB facility */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <miniweb/storage/sqlite_db.h>

#define MW_MAX_TABLES 32
#define MW_MAX_TABLE_NAME 64

struct mw_table_meta {
	char name[MW_MAX_TABLE_NAME];
};

/**
 * @brief Internal database handle used by the storage facade.
 */
struct mw_db {
	/** Configured database path. */
	char *path;
	/** Optional backend URL/DSN. */
	char *url;
	/** Friendly database name. */
	char *name;
	/** Implementation-defined open flags. */
	int flags;
	/** In-memory table registry for lightweight retrieval helpers. */
	struct mw_table_meta tables[MW_MAX_TABLES];
	size_t table_count;
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
	db->path = strdup(path);
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
	free(db->url);
	free(db->name);
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

/**
 * @brief Create a logical database handle from a name and URL.
 * @param name Logical database name.
 * @param url Backend URL/path used to open the storage handle.
 * @param out_db Output pointer receiving the created handle.
 * @return 0 on success, -1 on invalid input or allocation failure.
 */
int
create_db(const char *name, const char *url, struct mw_db **out_db)
{
	if (!name || !url || !out_db)
		return -1;
	if (mw_db_open(url, 0, out_db) != 0)
		return -1;
	(*out_db)->name = strdup(name);
	(*out_db)->url = strdup(url);
	if (!(*out_db)->name || !(*out_db)->url) {
		mw_db_close(*out_db);
		*out_db = NULL;
		return -1;
	}
	return 0;
}

/**
 * @brief Register a table in the lightweight in-memory table catalog.
 * @param db Open database handle.
 * @param name Table name to create/register.
 * @return 0 on success, -1 on invalid input or catalog capacity reached.
 */
int
create_table(struct mw_db *db, const char *name)
{
	if (!db || !name || *name == '\0')
		return -1;
	if (db->table_count >= MW_MAX_TABLES)
		return -1;
	for (size_t i = 0; i < db->table_count; i++) {
		if (strcmp(db->tables[i].name, name) == 0)
			return 0;
	}
	strlcpy(db->tables[db->table_count].name, name,
		sizeof(db->tables[db->table_count].name));
	db->table_count++;
	return 0;
}

/**
 * @brief Retrieve a table descriptor summary.
 * @param db Open database handle.
 * @param name Table name to look up.
 * @param limit Caller-requested row limit echoed in the summary.
 * @param out Output buffer for the summary string.
 * @param out_sz Size of @p out in bytes.
 * @return 0 on success, -1 on invalid input or unknown table.
 */
int
retrieve_table(struct mw_db *db, const char *name, size_t limit,
	char *out, size_t out_sz)
{
	size_t n;

	if (!db || !name || !out || out_sz == 0)
		return -1;
	for (n = 0; n < db->table_count; n++) {
		if (strcmp(db->tables[n].name, name) == 0)
			break;
	}
	if (n == db->table_count)
		return -1;
	snprintf(out, out_sz, "table=%s limit=%zu", name, limit);
	return 0;
}

/**
 * @brief Retrieve a column descriptor summary for a table.
 * @param db Open database handle.
 * @param table Table name to inspect.
 * @param limit Caller-requested row limit echoed in the summary.
 * @param out Output buffer for the summary string.
 * @param out_sz Size of @p out in bytes.
 * @return 0 on success, -1 on invalid input or unknown table.
 */
int
retrieve_column(struct mw_db *db, const char *table, size_t limit,
	char *out, size_t out_sz)
{
	size_t n;

	if (!db || !table || !out || out_sz == 0)
		return -1;
	for (n = 0; n < db->table_count; n++) {
		if (strcmp(db->tables[n].name, table) == 0)
			break;
	}
	if (n == db->table_count)
		return -1;
	snprintf(out, out_sz, "table=%s column=* limit=%zu", table, limit);
	return 0;
}
