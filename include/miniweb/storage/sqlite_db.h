/* sqlite_db.h - main db api */
#ifndef MINIWEB_STORAGE_SQLITE_DB_H
#define MINIWEB_STORAGE_SQLITE_DB_H

#include <stddef.h>

struct mw_db;

int mw_db_open(const char *path, int flags, struct mw_db **out_db);
void mw_db_close(struct mw_db *db);
int mw_db_exec_schema(struct mw_db *db, const char *schema_sql);

/** @brief Create a logical database handle.
 * @param name Logical DB name.
 * @param url DB backend URL/path.
 * @param out_db Output database handle.
 * @return 0 on success, -1 on failure.
 */
int create_db(const char *name, const char *url, struct mw_db **out_db);
/** @brief Register/create a table in the DB facade.
 * @param db Open DB handle.
 * @param name Table name.
 * @return 0 on success, -1 on failure.
 */
int create_table(struct mw_db *db, const char *name);
/** @brief Retrieve table information into a caller buffer.
 * @param db Open DB handle.
 * @param name Table name.
 * @param limit Max rows requested.
 * @param out Output summary buffer.
 * @param out_sz Buffer size.
 * @return 0 on success, -1 on failure.
 */
int retrieve_table(struct mw_db *db, const char *name, size_t limit,
	char *out, size_t out_sz);
/** @brief Retrieve column information into a caller buffer.
 * @param db Open DB handle.
 * @param table Table name.
 * @param limit Max rows requested.
 * @param out Output summary buffer.
 * @param out_sz Buffer size.
 * @return 0 on success, -1 on failure.
 */
int retrieve_column(struct mw_db *db, const char *table, size_t limit,
	char *out, size_t out_sz);

#endif
