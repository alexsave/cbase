#include "err.h"

const char *cb_strerror(int code) {
    switch (code) {
    case CB_OK:          return "CB_OK";
    case CB_E_NOTIMPL:   return "CB_E_NOTIMPL";
    case CB_E_NOT_FOUND: return "CB_E_NOT_FOUND";
    case CB_E_CONFLICT:  return "CB_E_CONFLICT";
    case CB_E_INVALID:   return "CB_E_INVALID";
    case CB_E_IO:        return "CB_E_IO";
    case CB_E_NOMEM:     return "CB_E_NOMEM";
    case CB_E_AUTH:      return "CB_E_AUTH";
    case CB_E_FORBIDDEN: return "CB_E_FORBIDDEN";
    case CB_E_FULL:      return "CB_E_FULL";
    case CB_E_DB:        return "CB_E_DB";
    case CB_E_STATE:     return "CB_E_STATE";
    default:             return code >= 0 ? "CB_OK(count)" : "CB_E_UNKNOWN";
    }
}
