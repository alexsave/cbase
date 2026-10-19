# shared/c's own tests, built once here for every product that runs them.
#
#   SHARED_TESTS := check_test text_util_test wire_check_test
#   include ../../shared/tools/c_rules.mk
#   include ../../shared/tools/shared_tests.mk
#   $(call SHARED_TESTS_IN,,build/obj/host,CC,CFLAGS)
#   $(call SHARED_TESTS_IN,asan_,build/obj/asan,CC,ASAN_FLAGS)
#   run: shared-tests
#   asan: shared-tests-asan
#
# SHARED_TESTS is set BEFORE the include: it is the prerequisite list of the
# two targets at the bottom of this file, and make reads a prerequisite list
# once, when it reads the rule.
#
# WHICH TESTS A PRODUCT RUNS IS THE PRODUCT'S (SHARED_TESTS, and
# SHARED_ASAN_TESTS when the sanitized list is shorter): a kernel runs the
# tests of what it links. WHAT EACH TEST IS BUILT FROM is this file's, because
# that is a fact about shared/c and it was written out in four Makefiles, with
# a second copy of every rule for the sanitizers.
#
#   $(1)  the prefix on the program's name: build/$(1)<test>
#   $(2)  the variant the objects are compiled in (c_rules.mk)
#   $(3)  the NAME of the compiler variable, for the link
#   $(4)  the NAME of the flags variable, for the link
#
# -lm on every link: three of them need it and it costs the rest nothing.

SHARED_C := $(C_ROOT)/shared/c

# The header-only ones (check, collapse, msg_stage, le_bytes, rng) and libc_test,
# which includes the libc it tests, are one file each.
SHARED_TEST_SRC_check_test            := test/check_test.c
SHARED_TEST_SRC_check_ok_test         := test/check_ok_test.c
SHARED_TEST_SRC_collapse_test         := collapse/collapse_test.c
SHARED_TEST_SRC_msg_stage_test        := msg_stage/msg_stage_test.c
SHARED_TEST_SRC_le_bytes_test         := le_bytes_test.c
SHARED_TEST_SRC_rng_test              := rng_test.c
SHARED_TEST_SRC_fnv_test              := fnv_test.c
SHARED_TEST_SRC_ease_test             := ease_test.c
SHARED_TEST_SRC_libc_test             := wasm/libc_test.c
SHARED_TEST_SRC_libm_test             := wasm/libm_test.c
SHARED_TEST_SRC_util_test             := util_test.c
SHARED_TEST_SRC_clock_test            := clock_test.c
SHARED_TEST_SRC_percentile_test       := stats/percentile_test.c
SHARED_TEST_SRC_b32_test              := b32_test.c b32.c
SHARED_TEST_SRC_mixrad_test           := mixrad_test.c mixrad.c
SHARED_TEST_SRC_sha256_test           := sha256_test.c sha256.c
SHARED_TEST_SRC_deal_rng_test         := deal_rng_test.c deal_rng.c
SHARED_TEST_SRC_text_util_test        := text_util/text_util.c text_util/text_util_test.c
SHARED_TEST_SRC_wire_check_test       := sha256.c wire_check/wire_check.c wire_check/wire_check_test.c
SHARED_TEST_SRC_msg_seat_tag_test     := msg_seat_tag/msg_seat_tag.c msg_seat_tag/msg_seat_tag_test.c
SHARED_TEST_SRC_msg_lobby_roster_test := msg_lobby_roster/msg_lobby_roster.c msg_lobby_roster/msg_lobby_roster_test.c
SHARED_TEST_SRC_stats_test            := stats/stats.c stats/stats_test.c
SHARED_TEST_SRC_b64_test              := b64/b64.c b64/b64_test.c
SHARED_TEST_SRC_hex_test              := hex/hex.c hex/hex_test.c
SHARED_TEST_SRC_msg_whole_test        := msg_whole/msg_whole.c msg_whole/msg_whole_test.c sha256.c b32.c \
                                         wire_check/wire_check.c msg_seat_tag/msg_seat_tag.c \
                                         msg_lobby_roster/msg_lobby_roster.c

SHARED_TEST_NAMES := check_test check_ok_test collapse_test msg_stage_test le_bytes_test rng_test fnv_test ease_test libc_test \
                     libm_test util_test clock_test percentile_test \
                     b32_test mixrad_test sha256_test deal_rng_test text_util_test \
                     wire_check_test msg_seat_tag_test msg_lobby_roster_test stats_test \
                     msg_whole_test b64_test hex_test

define SHARED_TEST_RULE
build/$(1)$(5): $$(call c_objs,$(2),$$(addprefix $$(SHARED_C)/,$$(SHARED_TEST_SRC_$(5))))
	@mkdir -p $$(@D)
	$$($(3)) $$($(4)) $$^ -lm -o $$@
endef

SHARED_TESTS_IN = $(foreach t,$(SHARED_TEST_NAMES),$(eval $(call SHARED_TEST_RULE,$(1),$(2),$(3),$(4),$(t))))

# A product that runs fewer of them sanitized says which; otherwise all.
SHARED_ASAN_TESTS ?= $(SHARED_TESTS)

.PHONY: shared-tests shared-tests-asan
shared-tests: $(addprefix build/,$(SHARED_TESTS))
	@for t in $(SHARED_TESTS); do echo "./build/$$t"; ./build/$$t || exit 1; done

shared-tests-asan: $(addprefix build/asan_,$(SHARED_ASAN_TESTS))
	@for t in $(SHARED_ASAN_TESTS); do echo "./build/asan_$$t"; ./build/asan_$$t || exit 1; done
