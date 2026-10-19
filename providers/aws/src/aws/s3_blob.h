// s3_blob.h - the credentials of the instance role (IMDSv2) and the cb_blob (util/blob.h) on an S3 bucket.
#ifndef CB_S3_BLOB_H
#define CB_S3_BLOB_H

#include "sigv4.h"

#include <stdint.h>

// The instance-metadata credentials document -> creds (region left as it was) and the expiry in ms since the
// epoch. CB_OK, or CB_E_INVALID when a field is missing or too long. The document is the metadata service's own
// format, which is not ours to change.
int cb_imds_parse(const char *doc, cb_aws_creds *out, int64_t *expiry_ms);

// Credentials for a call: the environment's, else the instance role's (fetched with a session token from the
// metadata service, IPv6 endpoint first, kept until five minutes before they expire). CBASE_IMDS_ENDPOINT
// ("host:port", plain HTTP) points the fetch elsewhere, for a stand-in.
int cb_aws_creds_get(cb_aws_creds *out);

#endif
