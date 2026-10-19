// unit_imds.c - the instance-metadata credentials document (aws/s3_blob.h cb_imds_parse).
#include "aws/s3_blob.h"
#include "t.h"
#include "util/err.h"

#include <string.h>

static void a_credentials_document_is_read_and_a_short_one_refused(void) {
    const char *doc = "{\n  \"Code\" : \"Success\",\n  \"AccessKeyId\" : \"ASIAEXAMPLE\",\n  \"SecretAccessKey\" : \"secret/example\",\n"
                      "  \"Token\" : \"tok+en==\",\n  \"Expiration\" : \"2026-10-19T18:45:00Z\"\n}";
    cb_aws_creds c;
    memset(&c, 0, sizeof c);
    int64_t exp = 0;
    T_EQ_INT(cb_imds_parse(doc, &c, &exp), CB_OK);
    T_CHECK(strcmp(c.access_key, "ASIAEXAMPLE") == 0);
    T_CHECK(strcmp(c.secret_key, "secret/example") == 0);
    T_CHECK(strcmp(c.session_token, "tok+en==") == 0);
    T_CHECK(exp == 1792435500000LL);   // 2026-10-19T18:45:00Z
    T_EQ_INT(cb_imds_parse("{\"AccessKeyId\" : \"A\"}", &c, &exp), CB_E_INVALID);
    T_EQ_INT(cb_imds_parse("", &c, &exp), CB_E_INVALID);
}

int main(void) {
    T_RUN(a_credentials_document_is_read_and_a_short_one_refused);
    return T_DONE();
}
