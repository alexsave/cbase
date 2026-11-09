// unit_startup_line.c - THE STARTUP LINE (ingress_process.h) as cb_startup_line writes it: the
// address as CBASE_LISTEN gave it, then the URL a person clicks, with a wildcard host's loopback
// in its place. smoke_boot.c reads the line a real serve prints; this holds every host's form.
#include "t.h"

#include "ingress_process.h"

#include <string.h>

static void the_line_for(const char *host, int port, const char *want) {
    char line[CB_STARTUP_LINE_MAX];
    int n = cb_startup_line(line, sizeof line, host, port);
    T_EQ_INT(n, (int)strlen(want));
    T_EQ_STR(n > 0 ? line : "", want);
}

static void the_address_then_the_url(void) {
    the_line_for("127.0.0.1", 8642, "cbase: listening 127.0.0.1:8642 http://127.0.0.1:8642/\n");
    the_line_for("localhost", 1, "cbase: listening localhost:1 http://localhost:1/\n");
    the_line_for("::1", 65535, "cbase: listening [::1]:65535 http://[::1]:65535/\n");
}

// Every address: the URL is the loopback's, which a browser on this machine can open.
static void a_wildcard_host_is_opened_on_the_loopback(void) {
    the_line_for("0.0.0.0", 8080, "cbase: listening 0.0.0.0:8080 http://127.0.0.1:8080/\n");
    the_line_for("::", 8080, "cbase: listening [::]:8080 http://[::1]:8080/\n");
}

// The largest host the config holds fits the documented bound, and a buffer too small is -1.
static void its_bound(void) {
    char host[64], line[CB_STARTUP_LINE_MAX];
    memset(host, 'h', sizeof host - 1);
    host[sizeof host - 1] = 0;
    T_CHECK(cb_startup_line(line, sizeof line, host, 65535) > 0);
    T_EQ_INT(cb_startup_line(line, 20, "127.0.0.1", 8642), -1);
}

int main(void) {
    T_RUN(the_address_then_the_url);
    T_RUN(a_wildcard_host_is_opened_on_the_loopback);
    T_RUN(its_bound);
    return T_DONE();
}
