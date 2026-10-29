/* Optional storage-library footprint/reopen probe, not a node storage backend.
 * Link with STORAGE_IN_NODE only in an isolated feasibility build. The normal
 * node targets do not compile this file and retain their existing size gate. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(STORAGE_SQLITE)
#include <sqlite3.h>
#elif defined(STORAGE_LMDB)
#include <lmdb.h>
#endif

static int storage_probe(void) {
    char dir[] = "/tmp/constella-storage-probe-XXXXXX", path[160];
    if (!mkdtemp(dir)) return 1;
    int failed = 0;
#if defined(STORAGE_SQLITE)
    sqlite3 *db = NULL;
    sqlite3_stmt *st = NULL;
    snprintf(path, sizeof path, "%s/probe.sqlite", dir);
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOFOLLOW, NULL) != SQLITE_OK) failed = 1;
    if (!failed && sqlite3_exec(db, "PRAGMA journal_mode=WAL; PRAGMA synchronous=FULL; PRAGMA cache_size=-16384; PRAGMA mmap_size=0; CREATE TABLE records(id BLOB PRIMARY KEY, raw BLOB NOT NULL)", NULL, NULL, NULL) != SQLITE_OK) failed = 1;
    if (!failed && sqlite3_prepare_v2(db, "INSERT INTO records VALUES(?1,?2)", -1, &st, NULL) != SQLITE_OK) failed = 1;
    if (!failed && (sqlite3_bind_blob(st, 1, "public-test-key", 15, SQLITE_STATIC) != SQLITE_OK ||
                   sqlite3_bind_blob(st, 2, "public-test-record", 18, SQLITE_STATIC) != SQLITE_OK || sqlite3_step(st) != SQLITE_DONE)) failed = 1;
    sqlite3_finalize(st); st = NULL;
    if (db && sqlite3_close(db) != SQLITE_OK) failed = 1;
    db = NULL;
    if (!failed && sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY | SQLITE_OPEN_NOFOLLOW, NULL) != SQLITE_OK) failed = 1;
    if (!failed && sqlite3_prepare_v2(db, "SELECT raw FROM records WHERE id=?1", -1, &st, NULL) != SQLITE_OK) failed = 1;
    if (!failed && (sqlite3_bind_blob(st, 1, "public-test-key", 15, SQLITE_STATIC) != SQLITE_OK || sqlite3_step(st) != SQLITE_ROW ||
                   sqlite3_column_bytes(st, 0) != 18 || memcmp(sqlite3_column_blob(st, 0), "public-test-record", 18))) failed = 1;
    sqlite3_finalize(st);
    if (db && sqlite3_close(db) != SQLITE_OK) failed = 1;
    printf("SQLite %s: transactional write/close/reopen/read %s\n", sqlite3_libversion(), failed ? "FAILED" : "passed");
#elif defined(STORAGE_LMDB)
    MDB_env *env = NULL;
    MDB_txn *tx = NULL;
    MDB_dbi dbi;
    MDB_val key = {15, "public-test-key"}, value = {18, "public-test-record"};
    if (mdb_env_create(&env) || mdb_env_set_mapsize(env, (size_t)1 << 30) || mdb_env_open(env, dir, MDB_NORDAHEAD, 0600)) failed = 1;
    if (!failed && (mdb_txn_begin(env, NULL, 0, &tx) || mdb_dbi_open(tx, NULL, 0, &dbi) || mdb_put(tx, dbi, &key, &value, 0))) failed = 1;
    if (!failed) { int r = mdb_txn_commit(tx); tx = NULL; if (r) failed = 1; }
    if (tx) mdb_txn_abort(tx);
    if (env) mdb_env_close(env);
    env = NULL; tx = NULL;
    if (!failed && (mdb_env_create(&env) || mdb_env_open(env, dir, MDB_RDONLY | MDB_NORDAHEAD, 0600))) failed = 1;
    if (!failed && (mdb_txn_begin(env, NULL, MDB_RDONLY, &tx) || mdb_dbi_open(tx, NULL, 0, &dbi) ||
                   mdb_get(tx, dbi, &key, &value) || value.mv_size != 18 || memcmp(value.mv_data, "public-test-record", 18))) failed = 1;
    if (tx) mdb_txn_abort(tx);
    if (env) mdb_env_close(env);
    printf("LMDB %s: transactional write/close/reopen/read %s\n", mdb_version(NULL, NULL, NULL), failed ? "FAILED" : "passed");
#else
    puts("No storage library selected");
#endif
    const char *files[] = {"probe.sqlite", "probe.sqlite-wal", "probe.sqlite-shm", "data.mdb", "lock.mdb"};
    for (unsigned i = 0; i < sizeof files / sizeof *files; i++) {
        snprintf(path, sizeof path, "%s/%s", dir, files[i]); unlink(path);
    }
    if (rmdir(dir)) failed = 1;
    return failed;
}

#ifdef STORAGE_IN_NODE
__attribute__((constructor)) static void probe_start(void) {
    const char *enabled = getenv("CONSTELLA_STORAGE_PROBE");
    if (enabled && !strcmp(enabled, "1")) exit(storage_probe());
}
#else
int main(void) { return storage_probe(); }
#endif
