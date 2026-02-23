#include <stdlib.h>

#include "../../include/miniweb/storage/sqlite_db.h"

struct mw_db {
	char *path;
	int flags;
};

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
	db->path = NULL;
	*out_db = db;
	return 0;
}

void
mw_db_close(struct mw_db *db)
{
	if (!db)
		return;
	free(db->path);
	free(db);
}

int
mw_db_exec_schema(struct mw_db *db, const char *schema_sql)
{
	if (!db || !schema_sql)
		return -1;
	return 0;
}
