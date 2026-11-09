# client/hosts/cdom/adapter.mk - the cdom adapter (cbc_cdom.h) as a make fragment a cdom app
# includes after cdom's own fragment (cdom.mk, at the cdom root) and client/client.mk.
#
# cdom's sources and flags are cdom.mk's alone (CDOM_CFLAGS, CDOM_LDFLAGS, CDOM_SRC and one
# CDOM_<NAME>_SRC per module), so a file cdom renames or adds is one edit, in cdom. This fragment
# names cbase's sources and flags, and which of cdom's modules the adapter calls, never a cdom file.
# The cdom the adapter has passed against is the pin README.md here names.
#
# With cdom.mk's APP_WASM rule: cdom.mk reads APP_SRC and APP_MODULES where it is included, and this
# fragment needs cdom.mk's variables first, so cdom.mk is included twice, for its variables and then,
# with APP_WASM set, for its rule:
#
#   CBASE := path/to/cbase                # REQUIRED: the cbase root, as the including Makefile reaches it
#   CDOM  := path/to/cdom                 # REQUIRED: the cdom checkout, as the including Makefile reaches it
#   include $(CDOM)/cdom.mk               # cdom's variables (no APP_WASM yet: no rule)
#   include $(CBASE)/client/client.mk
#   include $(CBASE)/client/hosts/cdom/adapter.mk
#   APP_WASM    := ui/out/app.wasm
#   APP_SRC     := ui/src/app.c $(CBC_SRC) $(CBC_B64_SRC) $(CBC_CDOM_SRC)
#   APP_MODULES := $(CBC_CDOM_MODULES)
#   APP_CFLAGS  := $(CBC_INC) $(CBC_CDOM_CFLAGS)
#   include $(CDOM)/cdom.mk               # again, APP_WASM set: the module's rule
#
# With a rule of the app's own, the same three includes once each, and cdom's sources from cdom.mk:
#
#   UI_SRC    := ui/src/app.c $(CBC_SRC) $(CBC_B64_SRC) $(CBC_CDOM_SRC) \
#                $(CDOM_SRC) $(CDOM_NET_SRC) $(CDOM_STORE_SRC) $(CDOM_RANDOM_SRC)
#   UI_CFLAGS := -Oz $(CDOM_CFLAGS) $(CBC_INC) $(CBC_CDOM_CFLAGS)
#   ui/out/app.wasm: $(UI_SRC)
#   	$(WASM_CC) $(UI_CFLAGS) $(CDOM_LDFLAGS) -o $@ $(UI_SRC)
#
# Each file is in one list: lib/b64 is client.mk's CBC_B64_SRC, which the adapter and the provider
# sign-in (CBC_OIDC_SRC) both need, so an app with both adds CBC_OIDC_SRC and nothing to dedupe.
#
# What it sets, every cbase path relative to CBASE as given:
#
#   CBC_CDOM_SRC      the adapter (cbc_cdom.c) and the shared host it runs on (CBC_HOST_SRC); it needs
#                     CBC_B64_SRC too (the session is kept in cdom's store as base64)
#   CBC_CDOM_CFLAGS   cbase's include flags for the adapter: the shared host's and this directory's
#                     (cdom's are in CDOM_CFLAGS)
#   CBC_CDOM_MODULES  the cdom modules the adapter calls, as cdom.mk's APP_MODULES names them
#
# cbase's own build (host.mk here) compiles and links the adapter from these same variables, with
# cdom.mk's APP_WASM rule, so an app's list and the one cbase tests are one list.

ifeq ($(strip $(CBC_SRC)),)
  $(error adapter.mk: include $$(CBASE)/client/client.mk before it)
endif
ifndef CDOM_CFLAGS
  $(error adapter.mk: include $$(CDOM)/cdom.mk (cdom's sources and flags) before it, from a cdom that has cdom.mk (README.md here: the pin))
endif

CBC_CDOM_SRC     := $(CBC_HOST_SRC) $(CBC_CB)client/hosts/cdom/cbc_cdom.c
CBC_CDOM_CFLAGS  := $(CBC_HOST_INC) -I$(CBC_CB)client/hosts/cdom
CBC_CDOM_MODULES := net store random
