// iot_data.h - publishing to AWS IoT Core (the iot-data plane).
//
// POST https://<endpoint>/topics/<topic, percent-encoded>?qos=0 with the raw
// message as the body, SigV4-signed for service "iotdata": no JSON either way
// (realtime/rt_iot.c's pushes). The endpoint is the account's iot:Data-ATS
// host (CBASE_IOT_ENDPOINT, "<prefix>-ats.iot.<region>.amazonaws.com"); the
// connection is kept alive by https.c's pool, so a warm container pays no
// handshake per publish. 200 published; anything else is logged.
#ifndef CB_IOT_DATA_H
#define CB_IOT_DATA_H

#include <stddef.h>

// The HTTP status IoT answered, or CB_E_IO (no credentials, the connection
// failed) / CB_E_INVALID (no endpoint or topic). The endpoint may carry a
// ":port" (a test's stand-in for AWS); it is always reached over TLS.
// AWS_ENDPOINT_URL_IOT_DATA_PLANE ("https://host[:port]", no path) replaces
// it when set, as the AWS SDKs' override does.
int cb_iot_publish(const char *endpoint, const char *topic, const void *data, size_t len);

#endif
