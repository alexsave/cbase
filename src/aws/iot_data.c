// iot_data.c - publishing to AWS IoT Core (iot_data.h).
#include "iot_data.h"

#include "aws_call.h"

#include "util/err.h"
#include "util/log.h"

#include <stdio.h>
#include <string.h>

#define IOT_TIMEOUT_MS 5000

int cb_iot_publish(const char *endpoint, const char *topic, const void *data, size_t len) {
    if (!endpoint || !*endpoint || !topic || !*topic || strchr(endpoint, '/')) return CB_E_INVALID;
    // "host" or "host:port" (a test's stand-in), always over TLS.
    char url[300], host[256], base[8];
    int port;
    bool tls;
    snprintf(url, sizeof url, "https://%s", endpoint);
    if (cb_aws_url_parse(url, host, sizeof host, &port, &tls, base, sizeof base) != CB_OK) return CB_E_INVALID;
    cb_buf path;
    cb_buf_init(&path);
    cb_buf_puts(&path, "/topics/");
    cb_aws_path_segment(&path, topic);
    cb_buf_puts(&path, "?qos=0");
    cb_https_response r;
    int rc = cb_aws_call("iotdata", "POST", host, port, true, cb_buf_cstr(&path), NULL, 0, data, len, IOT_TIMEOUT_MS, &r);
    if (rc == CB_OK) {
        rc = r.status;
        if (rc != 200) CB_WARN("iot", "publish to %s answered %d: %.200s", topic, rc, cb_buf_cstr(&r.body));
    } else {
        rc = CB_E_IO;
    }
    cb_https_response_free(&r);
    cb_buf_free(&path);
    return rc;
}
