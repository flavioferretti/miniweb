#include "../../include/miniweb/storage/sqlite_stmt.h"

struct mw_stmt {
	int reserved;
};

int
mw_stmt_prepare(struct mw_db *db, const char *sql, struct mw_stmt **out_stmt)
{
	(void)db;
	(void)sql;
	(void)out_stmt;
	return -1;
}

int
mw_bind_text(struct mw_stmt *stmt, int idx, const char *value)
{
	(void)stmt;
	(void)idx;
	(void)value;
	return -1;
}

int
mw_bind_int64(struct mw_stmt *stmt, int idx, int64_t value)
{
	(void)stmt;
	(void)idx;
	(void)value;
	return -1;
}

int
mw_bind_null(struct mw_stmt *stmt, int idx)
{
	(void)stmt;
	(void)idx;
	return -1;
}

int
mw_stmt_step(struct mw_stmt *stmt)
{
	(void)stmt;
	return -1;
}

void
mw_stmt_finalize(struct mw_stmt *stmt)
{
	(void)stmt;
}
