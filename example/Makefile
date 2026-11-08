# example/Makefile - the example app built as any product builds on cbase: include cbase.mk, say
# where the app is, name the providers. This one links the local provider alone, so it is what a
# product that runs on a person's own machine gets: `make -C example check` builds `cbase serve`
# with the app in it and runs the platform's local tests and the app's smoke, with no cloud account,
# no network service and no setup beyond the build.
#
#   make -C example            build/cbase
#   make -C example check      the compiler gate, lib/'s tests, every unit test, the smokes, and
#                              the link check
#   make -C example link-check no symbol of a provider this build does not link is in build/cbase
CBASE := ..
CBASE_PROVIDERS := local
EXAMPLE :=
include app.mk
include $(CBASE)/cbase.mk

# THE LINK CHECK: a local-only binary carries nothing of the cloud provider. Every external symbol
# of build/cbase is read (nm) and none may name the aws provider's parts: its store, its push
# transport, its signer, its runtime, its object store, its instance metadata. A provider file that
# reached this link by any path (a core call, a stray source root) puts its symbols here.
LINK_BANNED := aws|dynamo|sigv4|lambda|iot_|_iot|s3_|_s3|imds|runtime_api|cloudfront|eventbridge|ssm
.PHONY: link-check
link-check: build/$(APP_SERVE)
	@nm -g build/$(APP_SERVE) | awk '{print $$NF}' > build/link-check.syms
	@if grep -Ei '$(LINK_BANNED)' build/link-check.syms > build/link-check.hits; then head -20 build/link-check.hits; \
	  echo "link-check: build/$(APP_SERVE) (CBASE_PROVIDERS=$(CBASE_PROVIDERS)) holds $$(wc -l < build/link-check.hits | tr -d ' ') symbol(s) of a provider it does not link"; exit 1; fi
	@test $$(wc -l < build/link-check.syms) -gt 100 || { echo "link-check: build/$(APP_SERVE) shows too few symbols to judge"; exit 1; }
	@echo "link-check: build/$(APP_SERVE) holds no symbol of a provider it does not link ($$(wc -l < build/link-check.syms | tr -d ' ') symbols read)"
check: link-check
