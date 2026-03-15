#include <assert.h>
#include <stdio.h>

#include <miniweb/storage/sqlite_db.h>

/** @brief main function. */
int
main(void)
{
	struct mw_db *db = NULL;
	char out[128];

	assert(create_db("main", "./test.db", &db) == 0);
	assert(db != NULL);
	assert(create_table(db, "metrics") == 0);
	assert(retrieve_table(db, "metrics", 10, out, sizeof(out)) == 0);
	assert(retrieve_column(db, "metrics", 5, out, sizeof(out)) == 0);
	mw_db_close(db);

	puts("sqlite_db_test: ok");
	return 0;
}
