# client/hosts/cdom/adapter.mk - the cdom adapter (cbc_cdom.h) as a make fragment a cdom app
# includes after client/client.mk: the sources to compile into its wasm module and the flags.
#
#   CBASE := path/to/cbase                # REQUIRED: the cbase root, as the including Makefile reaches it
#   CDOM  := path/to/cdom                 # REQUIRED: the cdom checkout, as the including Makefile reaches it
#   include $(CBASE)/client/client.mk
#   include $(CBASE)/client/hosts/cdom/adapter.mk
#   APP_SRC += $(CBC_SRC) $(CBC_CDOM_SRC) $(CBC_CDOM_LINK_SRC)
#   CFLAGS  += $(CBC_INC) $(CBC_CDOM_CFLAGS)
#
# With CBC_OIDC_SRC as well, both lists carry lib/b64/b64.c: take $(sort ...) of them, so it is
# compiled once. The cdom the adapter has passed against is the pin README.md here names.
#
# What it sets, every cbase path relative to CBASE and every cdom path relative to CDOM, as given:
#
#   CBC_CDOM_SRC       the adapter (cbc_cdom.c), the shared host it runs on (CBC_HOST_SRC) and
#                      lib/b64, which keeps the session in cdom's store as base64
#   CBC_CDOM_INC       the include flags: the shared host's, the adapter's and cdom's src/
#   CBC_CDOM_CFLAGS    what a wasm32 compile of an app on cdom needs beyond CBC_INC: CBC_CDOM_INC and
#                      cdom's C library (-isystem <cdom>/src/libc, the program's string.h)
#   CBC_CDOM_LINK_SRC  cdom's sources the module links for the adapter: cdom.c, cdom_net.c,
#                      cdom_store.c, cdom_random.c and mini.c
#
# cbase's own build (host.mk here) compiles and links the adapter from these same variables, so
# an app's list and the one cbase tests are one list.

ifeq ($(strip $(CBC_SRC)),)
  $(error adapter.mk: include $$(CBASE)/client/client.mk before it)
endif
ifeq ($(strip $(CDOM)),)
  $(error adapter.mk: set CDOM to the cdom checkout (as the including Makefile reaches it) before including it)
endif

CBC_CDOM_SRC      := $(CBC_HOST_SRC) $(CBC_CB)client/hosts/cdom/cbc_cdom.c $(CBC_CB)lib/b64/b64.c
CBC_CDOM_INC      := $(CBC_HOST_INC) -I$(CBC_CB)client/hosts/cdom -I$(CDOM)/src
CBC_CDOM_CFLAGS   := -isystem $(CDOM)/src/libc $(CBC_CDOM_INC)
CBC_CDOM_LINK_SRC := $(addprefix $(CDOM)/src/,cdom.c cdom_net.c cdom_store.c cdom_random.c mini.c)
