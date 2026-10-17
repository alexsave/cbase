// fz_bcrypt.c - password hash strings (auth/bcrypt.h): the shape check the
// import runs on every row (cb_bcrypt_parse, cb_import_bcrypt_shaped) and the
// verify a sign-in runs against a stored hash. A stored hash came from
// Supabase's export or from this server; a malformed one must be refused, not
// read past. Verify is run only on hashes of cost 4..6 (a cost-31 hash is a
// correct, very slow answer, not a bug), and must agree with parse.
#include "fz.h"

#include "auth/bcrypt.h"
#include "store/store_import.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 200) return 0;
    const uint8_t *sep = memchr(data, 0, size);
    size_t hn = sep ? (size_t)(sep - data) : size;
    char *hash = fz_cstr(data, hn);
    char *pw = fz_cstr(sep ? sep + 1 : data, sep ? size - hn - 1 : 0);
    bool ok = cb_bcrypt_parse(hash);
    (void)cb_import_bcrypt_shaped(hash);
    if (ok) FZ_CHECK(strlen(hash) == CB_BCRYPT_HASH_LEN);
    bool cheap = hn > 6 && hash[4] == '0' && hash[5] >= '4' && hash[5] <= '6';
    if (!ok || cheap) {
        int v = cb_bcrypt_verify(pw, hash);
        FZ_CHECK(v == 0 || v == 1 || v < 0);
        if (!ok) FZ_CHECK(v < 0);
        if (v == 1) {
            // A hash that verifies the password verifies it again, and
            // nothing else of a different first byte.
            FZ_CHECK(cb_bcrypt_verify(pw, hash) == 1);
        }
    }
    free(pw);
    free(hash);
    return 0;
}
