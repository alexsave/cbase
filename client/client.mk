# client/client.mk - the cbase client core (cbase/client.h) as a make fragment a C app includes:
# the sources to compile into its own program (native or wasm32), the include path, and the flags.
#
#   CBASE := path/to/cbase                # REQUIRED: the cbase root, as the including Makefile reaches it
#   include $(CBASE)/client/client.mk
#   APP_SRC += $(CBC_SRC)                 # the core
#   APP_SRC += $(CBC_OIDC_SRC)            # and, for a provider's sign-in, its digests (with CBC_OIDC_CFLAGS)
#   CFLAGS  += $(CBC_INC) $(CBC_OIDC_CFLAGS)
#
# The core is SANS-IO and allocates nothing: the app's host owns the socket, HTTP, the timer and
# the clock (cbase/client.h says the contract, client/hosts/ holds hosts that keep it). What the
# core needs of the program it is linked into is CBC_REQUIRES and the three standard headers
# client.h names, and nothing else (client/test/requires_gate.sh holds it to that), so a
# freestanding wasm32 program links it with five functions of its own.
#
# What it sets, all relative to CBASE as given (never to the including Makefile's directory):
#
#   CBC_SRC          the core's sources
#   CBC_OIDC_SRC     what a build with CBC_OIDC adds: lib/'s SHA-256, base64url and hex. An app that
#                    already links one of them lists it once (the same files), never twice
#   CBC_INC          the include flags: cbase/client.h and the lib/ headers the core reads
#   CBC_OIDC_CFLAGS  -DCBC_OIDC: the provider sign-in (cbc_oidc_*, cbc_pkce)
#   CBC_CFLAGS       what the core needs of the compiler beyond the app's own flags: nothing (it is
#                    C11, -std=c11 or later, strict or GNU; the wasm module builds it -std=c11)
#   CBC_REQUIRES     the C library functions the core's objects may call
#
# The core is the same files cbase's own client/Makefile builds, tests and fuzzes: that Makefile
# includes this one, so there is one list.

ifeq ($(strip $(CBASE)),)
  $(error client.mk: set CBASE to the cbase root (as the including Makefile reaches it) before including it)
endif
# How a cbase path is spelled from the includer: "" when CBASE is ".", else CBASE with one slash.
CBC_CB := $(if $(filter .,$(CBASE)),,$(patsubst %/,%,$(CBASE))/)

CBC_SRC         := $(CBC_CB)client/src/client.c
CBC_OIDC_SRC    := $(CBC_CB)lib/sha256.c $(CBC_CB)lib/b64/b64.c $(CBC_CB)lib/hex/hex.c
CBC_INC         := -I$(CBC_CB)client/include -I$(CBC_CB)lib
CBC_OIDC_CFLAGS := -DCBC_OIDC
CBC_CFLAGS      :=
CBC_REQUIRES    := memcpy memmove memset memcmp strlen
