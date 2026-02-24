/* sqlite_db.h - main sql facility */
#ifndef MINIWEB_STORAGE_SQLITE_STMT_H
#define MINIWEB_STORAGE_SQLITE_STMT_H

#include <stddef.h>
#include <stdint.h>

struct mw_db;
struct mw_stmt;

int mw_stmt_prepare(struct mw_db *db, const char *sql, struct mw_stmt **out_stmt);
int mw_bind_text(struct mw_stmt *stmt, int idx, const char *value);
int mw_bind_int64(struct mw_stmt *stmt, int idx, int64_t value);
int mw_bind_null(struct mw_stmt *stmt, int idx);
int mw_stmt_step(struct mw_stmt *stmt);
void mw_stmt_finalize(struct mw_stmt *stmt);

#endif
