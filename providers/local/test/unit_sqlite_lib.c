// unit_sqlite_lib.c - the SQLite the local store runs on is upstream's, the one production links.
// Apple's /usr/lib/libsqlite3 carries code of its own (a purgeable page cache) that changes between
// macOS releases, and a test of this program's allocations would measure it (c_base/Makefile, SQLITE IS
// UPSTREAM'S). Held here:
//   - the library linked is the one whose header the store compiled against;
//   - it is not Apple's build (Apple's source id ends in "aapl").
#include "t.h"

#include <sqlite3.h>
#include <string.h>

static void the_library_is_the_headers(void) {
    T_EQ_STR(sqlite3_libversion(), SQLITE_VERSION);
    T_EQ_STR(sqlite3_sourceid(), SQLITE_SOURCE_ID);
}

static void the_library_is_not_apples(void) {
    const char *id = sqlite3_sourceid();
    size_t n = strlen(id);
    T_CHECK(!(n >= 4 && strcmp(id + n - 4, "aapl") == 0));
}

int main(void) {
    T_RUN(the_library_is_the_headers);
    T_RUN(the_library_is_not_apples);
    return T_DONE();
}
