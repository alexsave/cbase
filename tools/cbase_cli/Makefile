# cbase's deploy tool in C: tools/cbase's replacement, built here as build/cbase (the shell tool
# stays the default until it passes the same oracle; see the root Makefile's cbase-cli targets).
#
# It is a program of its own, like tools/structgen: it includes nothing from core/ or providers/
# (tools/repo_gate_test.sh), and its one header from outside its directory is lib/num_parse.h, the
# repository's one integer parser (DESIGN.md "Numbers from text").
C_ROOT := ../..
include $(C_ROOT)/tools/c_rules.mk

CLI_CFLAGS = -std=gnu11 -D_GNU_SOURCE -DCBC_TOOLS_DIR=\"$(CURDIR)/$(C_ROOT)/tools\" -O2 -g -Wall -Wextra -Werror -Wshadow -Wvla -Wformat=2
$(eval $(call C_VARIANT,build/obj,CC,CLI_CFLAGS))

CLI_SRC := cli.c conf.c main.c aws.c state.c stack.c vm.c vm_remote.c vm_boot.c static.c cert.c group.c deploy.c

# THE TEMPLATES, embedded (tools/embed.c, as cbase.mk embeds its own): the CloudFormation templates the
# stacks are rendered from are read from tools/cbase.d/templates at build time, so the binary needs no
# file beside it. The shell tool still reads the same files until milestone 7.
EMBED := build/embed
TEMPLATES := ../cbase.d/templates
$(EMBED): ../embed.c
	@mkdir -p $(@D)
	$(CC) -std=gnu11 -O2 -Wall -Wextra -Werror $< -o $@
build/gen/tpl_app.c: $(TEMPLATES)/app.yaml $(EMBED)
	@mkdir -p $(@D)
	$(EMBED) $< tpl_app_yaml > $@.new && mv $@.new $@
build/gen/tpl_group_vm.c: $(TEMPLATES)/group-vm.yaml $(EMBED)
	@mkdir -p $(@D)
	$(EMBED) $< tpl_group_vm_yaml > $@.new && mv $@.new $@
build/gen/tpl_group_static.c: $(TEMPLATES)/group-static.yaml $(EMBED)
	@mkdir -p $(@D)
	$(EMBED) $< tpl_group_static_yaml > $@.new && mv $@.new $@
CLI_SRC += build/gen/tpl_app.c build/gen/tpl_group_vm.c build/gen/tpl_group_static.c

# The per-file headers' table, static_rules.tsv, embedded the same way: static.c parses it as the shell does.
build/gen/static_rules_tsv.c: ../cbase.d/static_rules.tsv $(EMBED)
	@mkdir -p $(@D)
	$(EMBED) $< static_rules_tsv > $@.new && mv $@.new $@
CLI_SRC += build/gen/static_rules_tsv.c
.DEFAULT_GOAL := build/cbase


build/cbase: $(call c_objs,build/obj,$(CLI_SRC))
	@mkdir -p $(@D)
	@echo "  LINK $@"
	@$(CC) $^ -o $@

.PHONY: clean
clean:
	rm -rf build
