# example/minimal - the smallest cbase app with a page: a server with one route (server/minimal.c)
# and a page in C (page/page.c) that calls it through cbase's C client SDK on cdom, in one
# Makefile. A product copies this file and sets CBASE and CDOM to its two submodules.
#
#   make serve    build the server and the page, then run it: open the URL serve prints
#                 ("cbase: listening 127.0.0.1:8642 http://127.0.0.1:8642/"), or PORT=4100 make serve
#
# It is storeless (no store, no accounts), so it runs outside dev mode with no setting at all.
# client/hosts/cdom/host.mk's check-minimal-example builds it and checks it (check.sh here).
CBASE := ../..
# cdom as cbase fetches it, at the pin client/hosts/cdom/README.md names (make -C client
# check-host-cdom); a product's is its own submodule, CDOM := cdom.
CDOM  ?= ../../client/build/cdom

# ---- the server (cbase.mk's header) ----
CBASE_PROVIDERS := local
# Every .c under server/ is the server's, so the page lives apart, in page/. The server is
# build/minimal.
APP_SRC_ROOTS   := server
APP_SERVE       := minimal

# THE ORDER OF THE FOUR INCLUDES, the one every cbase doc shows (README.md, client/hosts/cdom/README.md,
# adapter.mk): cbase.mk first, because cdom.mk turns .SECONDEXPANSION on for every rule read after it
# and cbase.mk's rules are expanded as cbase's own build tests them only when read before it;
# adapter.mk last, because it needs cdom.mk's variables (CDOM_CFLAGS) and client.mk's (CBC_SRC), and
# stops, naming the missing one, when either is not read yet. cdom.mk's APP_WASM and APP_PAGE rules
# read APP_* when make builds, so those are set after the includes.
include $(CBASE)/cbase.mk
include $(CDOM)/cdom.mk
include $(CBASE)/client/client.mk
include $(CBASE)/client/hosts/cdom/adapter.mk

# ---- the page (cdom.mk's APP_WASM and APP_PAGE rules) ----
APP_WASM     := build/page/page.wasm
APP_SRC      := page/page.c $(CBC_SRC) $(CBC_B64_SRC) $(CBC_CDOM_SRC)
APP_MODULES  := $(CBC_CDOM_MODULES)
APP_CFLAGS   := -Wall -Wextra -Werror $(CBC_INC) $(CBC_CDOM_CFLAGS)
APP_LDFLAGS  := -Wl,--strip-all
# cdom's default page (cdom.mk's header), with the module bundled into it: one file, build/page/index.html.
APP_PAGE     := build/page/index.html
APP_TITLE    := cbase minimal

# make serve builds the page first (cbase.mk's header: a product's own serve lines).
serve: $(APP_WASM) $(APP_PAGE)
.PHONY: page
page: $(APP_WASM) $(APP_PAGE)
