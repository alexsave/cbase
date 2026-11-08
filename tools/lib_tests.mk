# lib/'s own tests, built once here for every build that runs them (cbase.mk).
#
#   LIB_TESTS := b64_test hex_test
#   include $(CB)tools/c_rules.mk
#   include $(CB)tools/lib_tests.mk
#   $(call LIB_TESTS_IN,,build/obj/host,CC,CFLAGS)
#   $(call LIB_TESTS_IN,asan_,build/obj/asan,CC,ASAN_FLAGS)
#   check: lib-tests
#   check-asan: lib-tests-asan
#
# LIB_TESTS is set BEFORE the include: it is the prerequisite list of the two targets at the bottom
# of this file, and make reads a prerequisite list once, when it reads the rule.
#
# WHICH TESTS A BUILD RUNS IS THE BUILD'S (LIB_TESTS, and LIB_ASAN_TESTS when the sanitized list is
# shorter): a program runs the tests of what it links. WHAT EACH TEST IS BUILT FROM is this file's,
# because that is a fact about lib/.
#
#   $(1)  the prefix on the program's name: build/$(1)<test>
#   $(2)  the variant the objects are compiled in (c_rules.mk)
#   $(3)  the NAME of the compiler variable, for the link
#   $(4)  the NAME of the flags variable, for the link
#
# The names are lib-tests and LIB_TEST_*, not shared-tests: a product that keeps shared helpers of
# its own, with a shared_tests.mk of its own, includes both without a clash. -lm on every link:
# some need it and it costs the rest nothing.

LIB_DIR ?= $(CB)lib

LIB_TEST_SRC_le_bytes_test     := le_bytes_test.c
LIB_TEST_SRC_rng_test          := rng_test.c
LIB_TEST_SRC_fnv_test          := fnv_test.c
LIB_TEST_SRC_clock_test        := clock_test.c
LIB_TEST_SRC_percentile_test   := stats/percentile_test.c
LIB_TEST_SRC_sha256_test       := sha256_test.c sha256.c
LIB_TEST_SRC_b64_test          := b64/b64.c b64/b64_test.c
LIB_TEST_SRC_hex_test          := hex/hex.c hex/hex_test.c
LIB_TEST_SRC_hist_test         := stats/hist.c stats/hist_test.c
LIB_TEST_SRC_proc_test         := proc.c proc_test.c
LIB_TEST_SRC_stream_frame_test := stream_frame_test.c

LIB_TEST_NAMES := le_bytes_test rng_test fnv_test clock_test percentile_test sha256_test \
                  b64_test hex_test hist_test proc_test stream_frame_test

define LIB_TEST_RULE
build/$(1)$(5): $$(call c_objs,$(2),$$(addprefix $$(LIB_DIR)/,$$(LIB_TEST_SRC_$(5))))
	@mkdir -p $$(@D)
	$$($(3)) $$($(4)) $$^ -lm -o $$@
endef

LIB_TESTS_IN = $(foreach t,$(LIB_TEST_NAMES),$(eval $(call LIB_TEST_RULE,$(1),$(2),$(3),$(4),$(t))))

# A build that runs fewer of them sanitized says which; otherwise all.
LIB_ASAN_TESTS ?= $(LIB_TESTS)

.PHONY: lib-tests lib-tests-asan
lib-tests: $(addprefix build/,$(LIB_TESTS))
	@for t in $(LIB_TESTS); do echo "./build/$$t"; ./build/$$t || exit 1; done

lib-tests-asan: $(addprefix build/asan_,$(LIB_ASAN_TESTS))
	@for t in $(LIB_ASAN_TESTS); do echo "./build/asan_$$t"; ./build/asan_$$t || exit 1; done
