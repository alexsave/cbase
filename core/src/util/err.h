// err.h - the error codes every c_base module returns.
//
// One numbering for the whole process, so a store refusal can travel up through
// the app layer to an HTTP handler without a translation table at each step.
// Zero is success, negative is a refusal. A function that returns a count or a
// length returns it as a non-negative int and one of these on failure.
#ifndef CB_ERR_H
#define CB_ERR_H

#define CB_OK            0
#define CB_E_NOTIMPL   (-1)   // a stub: the module has not been built yet
#define CB_E_NOT_FOUND (-2)   // no such row, game, user, route
#define CB_E_CONFLICT  (-3)   // a unique key is taken (an email, a game id)
#define CB_E_INVALID   (-4)   // the input does not parse or breaks a rule
#define CB_E_IO        (-5)   // a socket or file failed
#define CB_E_NOMEM     (-6)   // an allocation failed
#define CB_E_AUTH      (-7)   // no credentials, or ones that do not verify
#define CB_E_FORBIDDEN (-8)   // verified, but not allowed this
#define CB_E_FULL      (-9)   // a buffer, queue or table is at its cap
#define CB_E_DB       (-10)   // SQLite said no; the log says why
#define CB_E_STATE    (-11)   // called in the wrong state (closed, stopped, not loaded)
#define CB_E_BUSY     (-12)   // the store stayed throttled past its backoff: try again shortly

// A short constant name for logs ("CB_E_NOT_FOUND"), never NULL.
const char *cb_strerror(int code);

#endif
