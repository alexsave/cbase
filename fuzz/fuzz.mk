# fuzz.mk - the fuzz targets (fuzz/fz_*.c), included by c_base/Makefile.
#
#   make fuzz           build/fuzz/<target> for every fuzz/fz_<target>.c:
#                       libFuzzer + ASan + UBSan, every object (the kernel's
#                       included) rebuilt with coverage instrumentation
#   make fuzz-smoke     each target for FUZZ_TIME seconds (default 30), from
#                       a scratch copy of its checked-in corpus; a crash leaves
#                       its input in build/fuzz-crashes/<target>/ and fails
#   make fuzz-nightly   the same with FUZZ_TIME=600
#   make fuzz-replay    every file of fuzz/corpus/<target> through its target
#                       once, under the ordinary ASan build (any compiler, no
#                       libFuzzer): the corpus as a regression test
#   make fuzz-merge     each scratch corpus minimised back into fuzz/corpus/<target>
#   make fuzz-seeds     build/fuzz/seeds writes the seed inputs (fuzz/seeds.c)
#                       into fuzz/corpus/ (made from the real encoders and the
#                       tests' recorded fixtures)
#
# libFuzzer needs clang with -fsanitize=fuzzer. Apple's clang lacks it, so on
# macOS FUZZ_CC is Homebrew LLVM's; on Linux it is the clang on PATH.
#
# A target is one file; adding fuzz/fz_<name>.c adds the target to every rule
# above with no edit here. A corpus is fuzz/corpus/<name>/, created if missing.

ifeq ($(UNAME),Darwin)
  FUZZ_CC ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang) clang)
else
  FUZZ_CC ?= clang
endif
FUZZ_TIME ?= 30
FUZZ_JOBS ?= 1

FUZZ_SAN := -fsanitize=address,undefined,fuzzer-no-link -fno-sanitize-recover=undefined -fno-omit-frame-pointer
FUZZ_KERNEL_CFLAGS := $(filter-out -O3 -ffast-math -flto,$(KERNEL_CFLAGS)) -O1 -g $(FUZZ_SAN) -fno-sanitize=object-size
FUZZ_CB_CFLAGS := $(filter-out -O2,$(CB_CFLAGS)) -O1 $(FUZZ_SAN) -Ifuzz
# The replay build (no libFuzzer): the asan variant's objects, the harness too.
FUZZ_REPLAY_CFLAGS := $(ASAN_CB_CFLAGS) -Ifuzz

$(eval $(call C_VARIANT,build/obj/fuzz,FUZZ_CC,FUZZ_CB_CFLAGS))
$(eval $(call C_VARIANT,build/obj/fuzz-kernel,FUZZ_CC,FUZZ_KERNEL_CFLAGS))
$(eval $(call C_VARIANT,build/obj/fuzz-replay,CC,FUZZ_REPLAY_CFLAGS))

FUZZ_SRC     := $(sort $(wildcard fuzz/fz_*.c))
FUZZ_TARGETS := $(patsubst fuzz/fz_%.c,%,$(FUZZ_SRC))
# What a target links beside its own file: fuzz/lib_*.c (the harness helpers).
FUZZ_LIB     := $(sort $(wildcard fuzz/lib_*.c))
# Program sources a target replaces with its own (FUZZ_EXCLUDE_<target>):
# the sequence target makes the process's randomness a function of the input.
FUZZ_EXCLUDE_sequence := src/util/uuid.c
fuzz_lib = $(filter-out $(FUZZ_EXCLUDE_$(1)),$(LIB_SRC))

define FUZZ_PROG
build/fuzz/$(1): $$(call c_objs,build/obj/fuzz,fuzz/fz_$(1).c $$(FUZZ_LIB) $$(call fuzz_lib,$(1))) $$(call c_objs,build/obj/fuzz-kernel,$$(KERNEL_SRC))
	@mkdir -p $$(@D)
	@echo "  LINK $$@"
	@$$(FUZZ_CC) $$(FUZZ_CB_CFLAGS) -fsanitize=fuzzer $$^ -o $$@ $$(LDLIBS)
build/asan/fuzz/$(1): $$(call c_objs,build/obj/fuzz-replay,fuzz/fz_$(1).c fuzz/replay_main.c $$(FUZZ_LIB)) $$(call c_objs,build/obj/asan,$$(call fuzz_lib,$(1))) $$(call c_objs,build/obj/asan-kernel,$$(KERNEL_SRC))
	@mkdir -p $$(@D)
	@echo "  LINK $$@"
	@$$(CC) $$(ASAN_CB_CFLAGS) $$^ -o $$@ $$(LDLIBS)
endef
$(foreach t,$(FUZZ_TARGETS),$(eval $(call FUZZ_PROG,$(t))))

# The seed writer: an ordinary program under the asan build.
build/asan/fuzz/seeds: $(call c_objs,build/obj/fuzz-replay,fuzz/seeds.c $(FUZZ_LIB)) $(call c_objs,build/obj/asan,$(LIB_SRC)) $(call c_objs,build/obj/asan-kernel,$(KERNEL_SRC))
	@mkdir -p $(@D)
	@echo "  LINK $@"
	@$(CC) $(ASAN_CB_CFLAGS) $^ -o $@ $(LDLIBS)

.PHONY: fuzz fuzz-smoke fuzz-nightly fuzz-replay fuzz-merge fuzz-seeds
fuzz: $(addprefix build/fuzz/,$(FUZZ_TARGETS))

# libFuzzer's flags for every run: no leak report on macOS (LeakSanitizer does
# not run there), a 25 s per-input timeout, a 2 GB RSS cap.
FUZZ_FLAGS ?= -timeout=25 -rss_limit_mb=2048 -print_final_stats=1
# One target at a time; each run's new inputs go to a scratch corpus so the
# checked-in one changes only through fuzz-merge.
define FUZZ_RUN
	@fail=0; for t in $(FUZZ_TARGETS); do \
	  mkdir -p build/fuzz-work/$$t build/fuzz-crashes/$$t fuzz/corpus/$$t; \
	  cp -n fuzz/corpus/$$t/* build/fuzz-work/$$t/ 2>/dev/null; \
	  echo "== fuzz $$t ($(1) s)"; \
	  build/fuzz/$$t $(FUZZ_FLAGS) -max_total_time=$(1) -artifact_prefix=build/fuzz-crashes/$$t/ \
	    $(if $(filter-out 1,$(FUZZ_JOBS)),-fork=$(FUZZ_JOBS) -ignore_crashes=0,) \
	    build/fuzz-work/$$t fuzz/corpus/$$t > build/fuzz-work/$$t.log 2>&1 || { fail=1; echo "FUZZ FAILED: $$t"; tail -60 build/fuzz-work/$$t.log; }; \
	  grep -E '^stat::number_of_executed_units|^#[0-9]+.*DONE' build/fuzz-work/$$t.log | tail -1; \
	done; if [ $$fail -ne 0 ]; then exit 1; fi
endef
fuzz-smoke: fuzz
	$(call FUZZ_RUN,$(FUZZ_TIME))
fuzz-nightly: fuzz
	$(call FUZZ_RUN,600)

fuzz-replay: $(addprefix build/asan/fuzz/,$(FUZZ_TARGETS))
	@fail=0; for t in $(FUZZ_TARGETS); do \
	  mkdir -p fuzz/corpus/$$t; printf '%-20s ' $$t; build/asan/fuzz/$$t fuzz/corpus/$$t || fail=1; \
	done; if [ $$fail -ne 0 ]; then echo "FUZZ REPLAY FAILED"; exit 1; fi

fuzz-merge: fuzz
	@for t in $(FUZZ_TARGETS); do \
	  [ -d build/fuzz-work/$$t ] || continue; \
	  rm -rf build/fuzz-merged/$$t; mkdir -p build/fuzz-merged/$$t; \
	  build/fuzz/$$t -merge=1 $(FUZZ_FLAGS) build/fuzz-merged/$$t fuzz/corpus/$$t build/fuzz-work/$$t > build/fuzz-work/$$t.merge.log 2>&1 \
	    && rm -rf fuzz/corpus/$$t && mv build/fuzz-merged/$$t fuzz/corpus/$$t \
	    && echo "$$t: $$(ls fuzz/corpus/$$t | wc -l | tr -d ' ') inputs"; \
	done

fuzz-seeds: build/asan/fuzz/seeds
	build/asan/fuzz/seeds fuzz/corpus
