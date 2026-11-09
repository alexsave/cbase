# site.mk - a static site on S3 + CloudFront, as a make fragment.
# The production rule: static files from S3 + CloudFront, the API routes on the VM (or any origin
# a route names). cb_send_file is for local and desktop use only.
#
#   SITE_STACK  := mysite-web
#   SITE_DIR    := web/out
#   include $(CBASE)/providers/aws/ops/site/site.mk
#
# It needs neither cbase.mk nor CBASE, and does not care which providers CBASE_PROVIDERS links:
# a product whose API runs on the local provider deploys its site the same way.
# It finds its scripts beside itself. It needs bash and the AWS CLI, and runs on the caller's
# credentials: OIDC in CI (site_role.yaml), `aws login` on a Mac. No key is ever in a file.
#
# THE TARGETS
#   make deploy-web         upload SITE_DIR (site_deploy.sh): content-hashed files, other files,
#                           the router's page list, the html, deletes, one invalidation.
#                           PRUNE=1 also deletes content-hashed files this build does not have.
#                           FORCE=1 uploads every file again (after a change of rule).
#                           It never changes the template. Build the site first, with a line of
#                           the product's own: `deploy-web: web/out/index.html`.
#   make deploy-web-stack   create or update the stack (site_stack.sh deploy): drift detection
#                           first, refused when anything drifted; a change set, refused when it
#                           would replace or remove the distribution or the bucket.
#                           ACCEPT_DRIFT=1 overwrites drift; DRY_RUN=1 prints the change set,
#                           deletes it and executes nothing.
#   make site-template      the rendered template, on stdout (no AWS call).
#
# THE KNOBS (each optional but SITE_STACK, and SITE_DIR for deploy-web)
#   SITE_STACK       the stack's name.
#   SITE_DIR         the built site: a directory with index.html at its root.
#   SITE_NAME        the router's and the access control's name (default SITE_STACK).
#   SITE_PREFIX      a key prefix in the bucket (default none: the site is the bucket's root).
#   SITE_HASHED      content-hashed globs, space-separated, matched against the path from SITE_DIR
#                    with * crossing / (assets/*): immutable for a year, never deleted but by PRUNE=1.
#   SITE_RULES       the product's own rules file, read before site_rules.tsv (one table:
#                    glob TAB Content-Type TAB Cache-Control; html is revalidated, never immutable).
#   SITE_SHELL       the page an unknown one-segment path gets (/_shell.html), for an app that
#                    routes on the client; with it the router reads the page list, and a deeper
#                    unknown path gets /404.html. Without it every extensionless path gets .html.
#   SITE_REDIRECTS   302s, space-separated from=to (/old=/new).
#   SITE_ALIASES     the site's names, space-separated; SITE_CERT_ARN their ACM certificate (us-east-1).
#   SITE_BUCKET      the bucket's name; it may hold ${AWS::AccountId} (default CloudFormation's).
#   SITE_COMMENT     the distribution's comment (default "cbase site SITE_STACK").
#   SITE_PRICE_CLASS PriceClass_100 (default), PriceClass_200 or PriceClass_All.
#   SITE_ISOLATION   yes adds COOP same-origin and COEP credentialless to every S3 response.
#   SITE_ORIGINS     origins for routes, space-separated id=domain,option=value,...
#                    options: proto=http-only|https-only (default)|match-viewer, http=PORT,
#                    https=PORT, read=SECONDS, keepalive=SECONDS, ip=ipv4|ipv6|dualstack,
#                    header=NAME (a header CloudFront sends with the value of the environment's
#                    SITE_SECRET_<id>, never a knob). An origin no route uses is refused.
#   SITE_ROUTES      behaviours before the default (S3), in order, space-separated
#                    pattern=origin,option=value,... where origin is an id of SITE_ORIGINS or s3.
#                    options: cache=CachingOptimized (default)|CachingDisabled|<policy id>,
#                    request=AllViewer|AllViewerExceptHostHeader|<policy id> (default none),
#                    methods=read (default)|all, compress=yes (default)|no.
#                    An API route: /api/*=vm,cache=CachingDisabled,request=AllViewerExceptHostHeader,methods=all
#   AWS_REGION       us-east-1 (default; CloudFront reads certificates only there).

CBASE_SITE_DIR := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))

# The knobs reach the scripts through their environment, each named once here.
CBASE_SITE_KNOBS := SITE_STACK SITE_DIR SITE_NAME SITE_PREFIX SITE_HASHED SITE_RULES SITE_SHELL \
  SITE_REDIRECTS SITE_ALIASES SITE_CERT_ARN SITE_BUCKET SITE_COMMENT SITE_PRICE_CLASS \
  SITE_ISOLATION SITE_ORIGINS SITE_ROUTES
cbase_site_env = $(foreach k,$(CBASE_SITE_KNOBS),$(k)='$(subst ','\'',$($(k)))')

.PHONY: deploy-web deploy-web-stack site-template
deploy-web:
	$(cbase_site_env) bash '$(CBASE_SITE_DIR)/site_deploy.sh' $(if $(filter 1,$(PRUNE)),--prune) $(if $(filter 1,$(FORCE)),--force)
deploy-web-stack:
	$(cbase_site_env) bash '$(CBASE_SITE_DIR)/site_stack.sh' deploy $(if $(filter 1,$(ACCEPT_DRIFT)),--accept-drift) $(if $(filter 1,$(DRY_RUN)),--dry-run)
site-template:
	@$(cbase_site_env) bash '$(CBASE_SITE_DIR)/site_stack.sh' render
