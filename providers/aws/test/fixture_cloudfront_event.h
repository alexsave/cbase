// fixture_cloudfront_event.h - a Function URL event for a request that came
// through CloudFront: POST /auth/v1/signup with a JSON body, as Lambda
// delivered it on 2026-10-14 (captured from the WARN log of a failed decode).
// Only the viewer's address and location, the apikey and the body were
// replaced. CloudFront adds about 30 cloudfront-* and x-amz* headers, which
// take the event over 2 KB.
#ifndef CB_FIXTURE_CLOUDFRONT_EVENT_H
#define CB_FIXTURE_CLOUDFRONT_EVENT_H

static const char CLOUDFRONT_SIGNUP_EVENT[] =
    "{\"version\":\"2.0\",\"routeKey\":\"$default\",\"rawPath\":\"/auth/v1/signup\",\"rawQueryString\":\"\",\"headers\":{\"c"
    "loudfront-is-android-viewer\":\"false\",\"content-length\":\"93\",\"x-amzn-tls-version\":\"TLSv1.3\",\"cloudfron"
    "t-viewer-country\":\"US\",\"cloudfront-viewer-postal-code\":\"10001\",\"cloudfront-viewer-tls\":\"TLSv1.3:TLS_"
    "AES_128_GCM_SHA256:fullHandshake\",\"x-forwarded-port\":\"443\",\"via\":\"2.0 051f096b06d199a17be91748c92382"
    "c8.cloudfront.net (CloudFront)\",\"x-amzn-tls-cipher-suite\":\"TLS_AES_128_GCM_SHA256\",\"cloudfront-viewe"
    "r-country-name\":\"United States\",\"cloudfront-viewer-asn\":\"64500\",\"cloudfront-is-desktop-viewer\":\"true"
    "\",\"host\":\"7s44dwlftfznppvj2yny3tsdp40esnlg.lambda-url.us-east-1.on.aws\",\"content-type\":\"application/"
    "json\",\"cloudfront-viewer-city\":\"New York\",\"cloudfront-viewer-http-version\":\"2.0\",\"cloudfront-viewer-"
    "address\":\"203.0.113.7:50238\",\"apikey\":\"test-anon-key\",\"x-forwarded-proto\":\"https\",\"cloudfront-is-ios"
    "-viewer\":\"false\",\"cloudfront-viewer-metro-code\":\"501\",\"x-forwarded-for\":\"203.0.113.7\",\"cloudfront-vi"
    "ewer-country-region\":\"NY\",\"accept\":\"*/*\",\"cloudfront-viewer-time-zone\":\"America/New_York\",\"cloudfron"
    "t-is-smarttv-viewer\":\"false\",\"x-amzn-trace-id\":\"Root=1-6acfbbc5-70866caf694799b013c7d38d\",\"cloudfron"
    "t-viewer-longitude\":\"-73.99000\",\"cloudfront-is-tablet-viewer\":\"false\",\"cloudfront-forwarded-proto\":\""
    "https\",\"cloudfront-viewer-latitude\":\"40.75000\",\"cloudfront-viewer-country-region-name\":\"New York\",\"x"
    "-amz-cf-id\":\"C037HTwnNjLJ32mJEeRXSy-3m_QiZCJxGrvMarMOcxp2k06_F9hz-g==\",\"user-agent\":\"curl/8.7.1\",\"cl"
    "oudfront-is-mobile-viewer\":\"false\"},\"requestContext\":{\"accountId\":\"anonymous\",\"apiId\":\"7s44dwlftfznp"
    "pvj2yny3tsdp40esnlg\",\"domainName\":\"7s44dwlftfznppvj2yny3tsdp40esnlg.lambda-url.us-east-1.on.aws\",\"do"
    "mainPrefix\":\"7s44dwlftfznppvj2yny3tsdp40esnlg\",\"http\":{\"method\":\"POST\",\"path\":\"/auth/v1/signup\",\"pro"
    "tocol\":\"HTTP/1.1\",\"sourceIp\":\"198.51.100.72\",\"userAgent\":\"curl/8.7.1\"},\"requestId\":\"f0ec636b-0228-4e"
    "f9-9038-35ec931c3354\",\"routeKey\":\"$default\",\"stage\":\"$default\",\"time\":\"14/Oct/2026:17:28:38 +0000\",\""
    "timeEpoch\":1791998918003},\"body\":\"{\\\"email\\\":\\\"probecf1@probe.invalid\\\",\\\"password\\\":\\\"probe-pass-12"
    "3\\\",\\\"data\\\":{\\\"username\\\":\\\"probecf1\\\"}}\",\"isBase64Encoded\":false}";

#endif
