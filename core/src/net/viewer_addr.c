#include "viewer_addr.h"

#include "../../../../shared/c/num_parse.h"

#include <arpa/inet.h>
#include <string.h>

#define ADDR_MAX 63

bool cb_viewer_addr_parse(const char *s, char *out, size_t cap) {
    if (cap) out[0] = 0;
    if (!s || !cap) return false;
    size_t n = strnlen(s, ADDR_MAX + 1);
    if (n == 0 || n > ADDR_MAX) return false;
    const char *host, *port;
    char h[ADDR_MAX + 1];
    bool bracket = s[0] == '[';
    if (bracket) {
        const char *close = memchr(s, ']', n);
        if (!close || close == s + 1 || close + 1 >= s + n || close[1] != ':') return false;
        host = s + 1;
        memcpy(h, host, (size_t)(close - host));
        h[close - host] = 0;
        port = close + 2;
    } else {
        const char *colon = strrchr(s, ':');
        if (!colon || colon == s) return false;
        memcpy(h, s, (size_t)(colon - s));
        h[colon - s] = 0;
        port = colon + 1;
    }
    size_t pn = strlen(port);
    int64_t p;
    if (pn == 0 || pn > 5 || !num_parse_i64(port, pn, 1, 65535, &p)) return false;
    struct in_addr v4;
    struct in6_addr v6;
    // A bracket holds IPv6 only; unbracketed text is IPv4 unless it has a colon left.
    if (!bracket && inet_pton(AF_INET, h, &v4) == 1) return inet_ntop(AF_INET, &v4, out, (socklen_t)cap) != NULL;
    if (inet_pton(AF_INET6, h, &v6) == 1) return inet_ntop(AF_INET6, &v6, out, (socklen_t)cap) != NULL;
    if (cap) out[0] = 0;
    return false;
}
