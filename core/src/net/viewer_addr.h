// viewer_addr.h - the client address CloudFront vouches for, parsed strictly.
//
// CloudFront-Viewer-Address is "ip:port": "203.0.113.7:50238", or for IPv6
// "2001:db8:1:2::9:50238" (the last colon splits the port) or the bracketed
// "[2001:db8:1:2::9]:50238". It is untrusted input until the request has
// also carried the origin secret (config.h CBASE_ORIGIN_SECRET), which
// rate_limits.c cb_viewer_key checks before it calls this.
#ifndef CB_VIEWER_ADDR_H
#define CB_VIEWER_ADDR_H

#include <stdbool.h>
#include <stddef.h>

// Parses `s` (NUL-terminated, at most 63 bytes) into the bare address in
// canonical text (inet_ntop's), without the port, into out. false, out
// untouched or empty, for anything else: no port or one that is not 1 to
// 65535 in plain digits (num_parse.h), an address inet_pton refuses, a
// blank or any other stray byte, an unclosed bracket.
bool cb_viewer_addr_parse(const char *s, char *out, size_t cap);

#endif
