// unit_env_file.c - CBASE_ENV_FILE (env_file.h): each rule of the format, the environment's
// precedence, and every refusal's sentence with its line number.
#include "t.h"

#include "cbase/app.h"
#include "config.h"
#include "env_file.h"
#include "util/err.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_dir[256];
static char g_path[512];
static char g_err[512];

// Writes n bytes as the file and loads it: the load's code, its sentence in g_err.
static int load_n(const char *text, size_t n) {
    FILE *f = fopen(g_path, "wb");
    if (!f) return -100;
    fwrite(text, 1, n, f);
    fclose(f);
    g_err[0] = 0;
    return cb_env_file_load(g_path, g_err, sizeof g_err);
}
static int load(const char *text) { return load_n(text, strlen(text)); }

static const char *const KEYS[] = { "EF_A", "EF_B", "EF_C", "EF_D", "EF_E", "EF_F", "EF_G", "EF_H", "_ef9" };
static void clear(void) {
    for (size_t i = 0; i < sizeof KEYS / sizeof KEYS[0]; i++) unsetenv(KEYS[i]);
}
static const char *get(const char *k) {
    const char *v = getenv(k);
    return v ? v : "(unset)";
}

// A refusal: CB_E_INVALID, the file and the line's number in the sentence, and `what` too.
static void refused(const char *text, int line, const char *what) {
    clear();
    T_EQ_INT(load(text), CB_E_INVALID);
    char want[64];
    snprintf(want, sizeof want, "line %d", line);
    T_CHECK(strstr(g_err, want) != NULL);
    T_CHECK(strstr(g_err, g_path) != NULL);
    T_CHECK(strstr(g_err, what) != NULL);
    if (!strstr(g_err, want) || !strstr(g_err, what)) fprintf(stderr, "     the sentence: %s\n", g_err);
}

static void values_as_written(void) {
    clear();
    T_EQ_INT(load("EF_A=plain\n"
                  "EF_B=\"double quoted\"\n"
                  "EF_C='single \"inner\" quoted'\n"
                  "EF_D=$HOME/x ${EF_A} `id`\n"
                  "EF_E=a#not-a-comment=b\n"
                  "EF_F=\n"
                  "EF_G=\"\"\n"
                  "_ef9=crlf\r\n"
                  "EF_H=last line, no newline"), CB_OK);
    T_EQ_STR(g_err, "");
    T_EQ_STR(get("EF_A"), "plain");
    T_EQ_STR(get("EF_B"), "double quoted");            // the pair goes, and nothing else
    T_EQ_STR(get("EF_C"), "single \"inner\" quoted");
    T_EQ_STR(get("EF_D"), "$HOME/x ${EF_A} `id`");     // no expansion of any kind
    T_EQ_STR(get("EF_E"), "a#not-a-comment=b");        // '#' and '=' in a value are its own
    T_EQ_STR(get("EF_F"), "");
    T_EQ_STR(get("EF_G"), "");
    T_EQ_STR(get("_ef9"), "crlf");                     // a "\r\n" end is a "\n" one
    T_EQ_STR(get("EF_H"), "last line, no newline");
    clear();
}

static void comments_and_blanks(void) {
    clear();
    T_EQ_INT(load("# a comment\n\n   \n\t\n#EF_A=commented out\nEF_B=x\n\n"), CB_OK);
    T_EQ_STR(get("EF_A"), "(unset)");
    T_EQ_STR(get("EF_B"), "x");
    T_EQ_INT(load(""), CB_OK);   // an empty file sets nothing and is no error
    clear();
}

static void the_environment_wins(void) {
    clear();
    setenv("EF_A", "from the shell", 1);
    setenv("EF_B", "", 1);   // set, if empty: still the environment's
    T_EQ_INT(load("EF_A=from the file\nEF_B=from the file\nEF_C=from the file\n"), CB_OK);
    T_EQ_STR(get("EF_A"), "from the shell");
    T_EQ_STR(get("EF_B"), "");
    T_EQ_STR(get("EF_C"), "from the file");
    clear();
}

static void refusals_name_the_line(void) {
    refused("EF_A=1\nEF_B\n", 2, "no '='");
    refused("# c\nEF_A =1\n", 2, "blank before");
    refused("EF_A= 1\n", 1, "blank after");
    refused("\n\nEF_A=\t1\n", 3, "blank after");
    refused("EF_A=1\n1EF=2\n", 2, "\"1EF\" is not a key");
    refused("export EF_A=1\n", 1, "\"export EF_A\" is not a key");
    refused(" EF_A=1\n", 1, "\" EF_A\" is not a key");   // nothing before the key
    refused("EF-A=1\n", 1, "is not a key");
    refused("=1\n", 1, "is not a key");
    refused("EF_A=\"open\n", 1, "double quote");
    refused("EF_A='open\"\n", 1, "single quote");
    refused("EF_A=\"\n", 1, "double quote");              // a lone quote is no pair
    refused("EF_A=\"v\" # trailing\n", 1, "double quote");
    refused("EF_A=v \n", 1, "ends in a blank");
    refused("EF_A=1\nEF_B=2\nEF_A=3\n", 3, "sets EF_A again (line 1 did)");
    clear();
    T_EQ_INT(load_n("EF_A=1\nEF_B=a\0b\n", sizeof "EF_A=1\nEF_B=a\0b\n" - 1), CB_E_INVALID);
    T_CHECK(strstr(g_err, "line 2 holds a NUL byte") != NULL);
}

// A refused file sets nothing, not even its good lines before the bad one.
static void a_refused_file_sets_nothing(void) {
    clear();
    T_EQ_INT(load("EF_A=1\nEF_B=2\nbad line\n"), CB_E_INVALID);
    T_EQ_STR(get("EF_A"), "(unset)");
    T_EQ_STR(get("EF_B"), "(unset)");
}

static void unreadable_and_oversized(void) {
    char missing[600];
    snprintf(missing, sizeof missing, "%s/no-such.env", g_dir);
    T_EQ_INT(cb_env_file_load(missing, g_err, sizeof g_err), CB_E_INVALID);
    T_CHECK(strstr(g_err, missing) != NULL && strstr(g_err, "cannot be read") != NULL);
    size_t n = CB_ENV_FILE_MAX + 1;
    char *big = malloc(n);
    T_CHECK(big != NULL);
    if (!big) return;
    memset(big, '#', n);
    T_EQ_INT(load_n(big, n), CB_E_INVALID);
    T_CHECK(strstr(g_err, "larger than") != NULL);
    T_EQ_INT(load_n(big, n - 1), CB_OK);   // one comment line, at the cap
    free(big);
}

// Through cb_config_load: the file is read before the configuration, so it may hold CBASE_*
// settings as well as the app's own; unset or empty, no file; a bad one fails the load.
static void read_before_the_configuration(void) {
    clear();
    unsetenv("CBASE_REQ_WORKERS");
    unsetenv("CBASE_ENV_FILE");
    T_EQ_INT(load("CBASE_REQ_WORKERS=5\nEF_A=for the app\n"), CB_OK);
    unsetenv("CBASE_REQ_WORKERS");
    clear();
    static const cb_app APP = { .name = "envfile" };
    cb_config c;
    char err[512] = "";
    T_EQ_INT(cb_config_load(&c, &APP, err, sizeof err), CB_OK);
    T_EQ_INT(c.req_workers, 1);
    T_EQ_STR(get("EF_A"), "(unset)");
    setenv("CBASE_ENV_FILE", "", 1);
    T_EQ_INT(cb_config_load(&c, &APP, err, sizeof err), CB_OK);
    T_EQ_STR(get("EF_A"), "(unset)");
    setenv("CBASE_ENV_FILE", g_path, 1);
    T_EQ_INT(cb_config_load(&c, &APP, err, sizeof err), CB_OK);
    T_EQ_INT(c.req_workers, 5);
    T_EQ_STR(get("EF_A"), "for the app");
    unsetenv("CBASE_REQ_WORKERS");
    clear();
    T_EQ_INT(load("EF_A=1\nEF_B 2\n"), CB_E_INVALID);
    clear();
    T_EQ_INT(cb_config_load(&c, &APP, err, sizeof err), CB_E_INVALID);
    T_CHECK(strstr(err, "line 2") != NULL);
    unsetenv("CBASE_ENV_FILE");
    clear();
}

int main(void) {
    const char *t = getenv("TMPDIR");
    snprintf(g_dir, sizeof g_dir, "%s/envfile-XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(g_dir)) { perror("mkdtemp"); return 1; }
    snprintf(g_path, sizeof g_path, "%s/.env.local", g_dir);
    T_RUN(values_as_written);
    T_RUN(comments_and_blanks);
    T_RUN(the_environment_wins);
    T_RUN(refusals_name_the_line);
    T_RUN(a_refused_file_sets_nothing);
    T_RUN(unreadable_and_oversized);
    T_RUN(read_before_the_configuration);
    unlink(g_path);
    rmdir(g_dir);
    return T_DONE();
}
