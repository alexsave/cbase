// tls.h - what the platform's outbound clients (https.c, h2.c) share: one TCP
// dial with a deadline, and one TLS client context, so both verify a peer the
// same way.
//
// THE CONTEXT: TLS 1.2 or newer, the peer's certificate verified against a CA
// bundle. The bundle is, in order: the file given, $CBASE_CA_FILE, OpenSSL's
// own file when it exists ($SSL_CERT_FILE, else the one its build names:
// Homebrew's on a Mac, the distribution's on Linux), the first that exists of
// /etc/pki/tls/certs/ca-bundle.crt (Fedora and its kin, among them a one-shot
// provider's runtime), /etc/ssl/certs/ca-certificates.crt (Debian, Alpine) and
// /etc/ssl/cert.pem, and else OpenSSL's default paths. OpenSSL's file comes
// before the list because a Mac also has /etc/ssl/cert.pem, Apple's lagging
// copy; the list is there because the static Alpine build's file is absent on
// the runtime it ships to. The host name is checked per connection
// (cb_tls_bind_host).
#ifndef CB_TLS_H
#define CB_TLS_H

#include <openssl/ssl.h>

#include <stdbool.h>
#include <stdint.h>

// A client context. ca_file NULL or "": the search above. alpn: the protocol
// list in wire form ("\x02h2"), or NULL for none. NULL when the bundle does
// not load (the log says which) or on no memory.
SSL_CTX *cb_tls_client_ctx(const char *ca_file, const unsigned char *alpn, unsigned alpn_len);

// SNI and the name the certificate must carry. CB_OK, or CB_E_IO when the name
// cannot be set: then no connection is made at all, rather than one whose
// certificate is checked against nothing.
int cb_tls_bind_host(SSL *ssl, const char *host);

// A TCP connection to host:port by deadline_ms (cb_mono_ms), trying each
// address in turn, with SO_NOSIGPIPE where there is one. The descriptor is
// blocking unless nonblocking. -1 when none connects in time.
int cb_net_dial(const char *host, int port, int64_t deadline_ms, bool nonblocking);

#endif
