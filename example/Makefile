# example/Makefile - the example app built as any product builds on cbase: include cbase.mk, say
# where the app is, name the providers. This one links the local provider alone, so it is what a
# product that runs on a person's own machine gets: `make -C example check` builds `cbase serve`
# with the app in it and runs the platform's local tests and the app's smoke, with no cloud account,
# no network service and no setup beyond the build.
#
#   make -C example            build/cbase
#   make -C example serve      build/cbase, then `cbase serve` from example/ on 127.0.0.1:3000 (or
#                              PORT), with example/.env.local as CBASE_ENV_FILE when it exists
#                              (cbase.mk's header: APP_SERVE_DIR, APP_ENV_FILE, APP_SERVE_LISTEN)
#   make -C example check      the compiler gate, lib/'s tests, every unit test, the smokes, and
#                              the link check
#   make -C example link-check no symbol of a provider this build does not link is in build/cbase
CBASE := ..
CBASE_PROVIDERS := local
EXAMPLE :=
include app.mk
include $(CBASE)/cbase.mk

# THE LINK CHECK: a local-only binary carries nothing of the cloud provider. Every global symbol
# of build/cbase is read (nm) and none may be on the aws word list (tools/symbol_gate.sh: its
# store, push transport, signer, runtime, object store, instance metadata). A provider file that
# reached this link by any path (a core call, a stray source root) puts its symbols here. cbase's
# own check runs the same gate over this binary with every aws object's symbols banned as well
# (the top-level Makefile's symbol-gate), which a local-only build cannot do: it compiles none.
.PHONY: link-check
link-check: build/$(APP_SERVE)
	bash $(CBASE)/tools/symbol_gate.sh build/$(APP_SERVE)
check: link-check
