// apigw_mgmt.h - pushes to API Gateway WebSocket connections.
//
// POST https://<CBASE_WS_ENDPOINT host>/<stage>/@connections/<connectionId>
// with the raw frame text as the body, SigV4-signed for service
// "execute-api". 200 delivered; 410 Gone means the client left (the caller
// deletes its CONN row); anything else is logged.
#ifndef CB_APIGW_MGMT_H
#define CB_APIGW_MGMT_H

#include <stddef.h>

#define CB_APIGW_GONE 410

// The HTTP status API Gateway answered, or CB_E_IO / CB_E_NOTIMPL.
int cb_apigw_post(const char *endpoint, const char *connection_id, const void *data, size_t len);
// Closes a connection (DELETE on the same path).
int cb_apigw_delete(const char *endpoint, const char *connection_id);

#endif
