// lib_rt_token.h - the fake token check the realtime tests over a real socket
// share (unit_stream.c, unit_rt_local.c): "tok-<uid>" verifies as <uid> (its
// username "U-<uid>"), anything else is CB_E_AUTH. rtt_install() swaps it into
// realtime's dependencies (realtime/rt_deps.h). Thread-safe: it keeps nothing.
#ifndef CB_TEST_RT_TOKEN_H
#define CB_TEST_RT_TOKEN_H

void rtt_install(void);
// "tok-<uid>@<session>" names the session; a session named here is one session_live refuses.
extern char rtt_revoked[];

#endif
