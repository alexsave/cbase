// unit_viewer_addr.c - cb_viewer_addr_parse (net/viewer_addr.h): the header CloudFront
// vouches for, parsed strictly. A table of what it takes and what it refuses.
#include "t.h"

#include "net/viewer_addr.h"

#include <string.h>

static void good(const char *in, const char *want) {
    char out[64];
    memset(out, 'x', sizeof out);
    bool ok = cb_viewer_addr_parse(in, out, sizeof out);
    if (!ok) fprintf(stderr, "     refused: [%s]\n", in);
    T_CHECK(ok);
    T_EQ_STR(out, want);
}

static void bad(const char *in) {
    char out[64];
    memset(out, 'x', sizeof out);
    bool ok = cb_viewer_addr_parse(in, out, sizeof out);
    if (ok) fprintf(stderr, "     taken: [%s] as [%s]\n", in, out);
    T_CHECK(!ok);
    T_EQ_INT(out[0], 0);   // a refusal leaves the output empty
}

static void what_it_takes(void) {
    good("203.0.113.7:50238", "203.0.113.7");
    good("203.0.113.7:1", "203.0.113.7");
    good("203.0.113.7:65535", "203.0.113.7");
    good("203.0.113.7:00443", "203.0.113.7");                   // plain digits, leading zeros and all
    good("2001:db8:1:2::9:50238", "2001:db8:1:2::9");            // the last colon splits the port
    good("2001:db8:1:2:3:4:5:6:443", "2001:db8:1:2:3:4:5:6");
    good("[2001:db8:1:2::9]:50238", "2001:db8:1:2::9");          // bracketed
    good("[2001:DB8::1]:443", "2001:db8::1");                    // canonical text, lower case
    good("[::1]:80", "::1");
    good("::1:80", "::1");
    good("0.0.0.0:1", "0.0.0.0");
}

static void what_it_refuses(void) {
    bad("");                          // empty
    bad("203.0.113.7");               // no port
    bad("203.0.113.7:");              // empty port
    bad(":443");                      // no address
    bad("203.0.113.7:0");             // port 0
    bad("203.0.113.7:65536");         // past the top
    bad("203.0.113.7:123456");        // five digits is the most
    bad("203.0.113.7:000080");        // even when the value is small
    bad("203.0.113.7:12x");           // not plain digits
    bad("203.0.113.7:+80");
    bad("203.0.113.7:-80");
    bad("203.0.113.7: 80");
    bad("203.0.113.7:80 ");           // trailing blank
    bad(" 203.0.113.7:80");           // leading blank
    bad("203.0.113.7:80\n");
    bad("203.0.113.256:80");          // not an address
    bad("203.0.113:80");
    bad("not-an-address:80");
    bad("[203.0.113.7]:80");          // a bracket holds IPv6 only
    bad("[2001:db8::1]");             // bracketed, no port
    bad("[2001:db8::1]:");            // bracketed, empty port
    bad("[2001:db8::1]80");           // no colon after the bracket
    bad("[2001:db8::1]x80");          // a stray byte there
    bad("[2001:db8::1:80");           // unclosed
    bad("[]:80");                     // empty bracket
    bad("2001:db8::1");               // bare v6 with no port: "1" is the port and "2001:db8:" no address
    bad("[2001:db8::1]:80:90");       // junk after the bracket's port
    bad("2001:db8:::1:80");           // not an address
    bad("203.0.113.7:80:90");         // "203.0.113.7:80" is no address
}

static void its_length_and_its_buffer(void) {
    // 63 bytes at most: a 64-byte string is refused whatever it holds.
    char s[80];
    memset(s, '1', sizeof s);
    s[64] = 0;
    bad(s);
    // A 63-byte one that is no address is refused, not mistaken for a long one.
    s[63] = 0;
    bad(s);
    // The output is sized by the caller: too small for the canonical text is a refusal, empty.
    char out[8] = "xxxxxxx";
    T_CHECK(!cb_viewer_addr_parse("[2001:db8:1:2::9]:443", out, sizeof out));
    T_EQ_INT(out[0], 0);
    T_CHECK(cb_viewer_addr_parse("203.0.113.7:1", out, sizeof out) == false);   // 11 bytes + NUL > 8
    T_CHECK(cb_viewer_addr_parse("1.2.3.4:1", out, sizeof out));                // 7 + NUL fits exactly
    T_EQ_STR(out, "1.2.3.4");
    // No room at all and no string at all.
    T_CHECK(!cb_viewer_addr_parse("1.2.3.4:1", out, 0));
    T_CHECK(!cb_viewer_addr_parse(NULL, out, sizeof out));
    T_EQ_INT(out[0], 0);
}

int main(void) {
    T_RUN(what_it_takes);
    T_RUN(what_it_refuses);
    T_RUN(its_length_and_its_buffer);
    return T_DONE();
}
