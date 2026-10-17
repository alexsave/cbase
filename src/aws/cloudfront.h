// cloudfront.h - CreateInvalidation, the one CloudFront call c_base makes.
//
// POST https://cloudfront.amazonaws.com/2020-05-31/distribution/<id>/invalidation
// with the InvalidationBatch XML (json_surface.h cf_invalidation_xml), SigV4
// for service "cloudfront" in us-east-1 (a global service signs there from
// any region). 201 Created is success. AWS_ENDPOINT_URL_CLOUDFRONT
// ("https://host[:port]"), the AWS SDKs' own override, replaces the endpoint.
//
// Each path invalidated is billed past the first 1,000 a month, whichever
// distribution; app/edge.h decides when to call this (DESIGN.md "Edge cache").
#ifndef CB_CLOUDFRONT_H
#define CB_CLOUDFRONT_H

#include "util/str.h"

// The request, without sending it: the path (after the host) and the body.
// Pure, so the bytes are checked against a recorded request in the unit tests.
// CB_OK, or CB_E_INVALID (no distribution, no paths), CB_E_NOMEM.
int cb_cloudfront_invalidation(const char *distribution, const char *const *paths, int n, const char *caller_ref,
                               cb_buf *path, cb_buf *body);

// Sends it. CB_OK when CloudFront accepted it (201), else CB_E_IO (logged).
int cb_cloudfront_invalidate(const char *distribution, const char *const *paths, int n, const char *caller_ref);

#endif
