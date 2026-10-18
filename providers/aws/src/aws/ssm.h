// ssm.h - the function's secrets from SSM Parameter Store (security finding M5).
//
// The JWT secret and the admin token are SecureString parameters under one
// prefix (ops/cbase.yaml SecretsPrefix, "/cbase/"):
//   <prefix>jwt-secret     CBASE_JWT_SECRET
//   <prefix>admin-token    CBASE_ADMIN_TOKEN
// read once, when a container starts, with the function role's
// ssm:GetParameters on that prefix (and the AWS-managed aws/ssm key's decrypt,
// which SSM does for it). So they are no longer in the function's
// environment, which anyone who may read the function's configuration reads
// (the GitHub deploy role must, for CloudFormation's GetFunction). One
// GetParameters a cold start: two KMS decrypts, inside KMS's free 20,000
// requests a month at any cold-start rate this function has.
//
// The origin secret (CBASE_ORIGIN_SECRET) is not here: CloudFront must hold
// it in the distribution's configuration, which the deploy role reads
// anyway, so SSM would protect nothing, and it stays the environment's.
#ifndef CB_SSM_H
#define CB_SSM_H

#include "config.h"

#include <stddef.h>

// The two secrets under cfg->secrets_ssm into cfg, each one that SSM lists
// (a missing one is left as it was: config.h decides what is required).
// AWS_ENDPOINT_URL_SSM overrides the endpoint (the tests). CB_OK, or CB_E_IO
// (no credentials, the call failed, a value that does not fit or holds a NUL)
// with a sentence in err that never holds a value.
int cb_ssm_load_secrets(cb_config *cfg, char *err, size_t cap);

#endif
