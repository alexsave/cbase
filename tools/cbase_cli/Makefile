# cbase's deploy tool in C: tools/cbase's replacement, built here as build/cbase (the shell tool
# stays the default until it passes the same oracle; see the root Makefile's cbase-cli targets).
#
# It is a program of its own, like tools/structgen: it includes nothing from core/ or providers/
# (tools/repo_gate_test.sh), and its one header from outside its directory is lib/num_parse.h, the
# repository's one integer parser (DESIGN.md "Numbers from text").
C_ROOT := ../..
include $(C_ROOT)/tools/c_rules.mk

CLI_CFLAGS = -std=gnu11 -D_GNU_SOURCE -O2 -g -Wall -Wextra -Werror -Wshadow -Wvla -Wformat=2
$(eval $(call C_VARIANT,build/obj,CC,CLI_CFLAGS))

CLI_SRC := cli.c conf.c main.c aws.c state.c stack.c

build/cbase: $(call c_objs,build/obj,$(CLI_SRC))
	@mkdir -p $(@D)
	@echo "  LINK $@"
	@$(CC) $^ -o $@

.PHONY: clean
clean:
	rm -rf build
