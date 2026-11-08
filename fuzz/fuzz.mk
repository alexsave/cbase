# fuzz.mk - the fuzz targets, included by cbase.mk. A target lives on the side
# whose code it attacks (FUZZ_ROOTS: core/fuzz, providers/<p>/fuzz for each
# linked provider, and the product's APP_FUZZ_ROOTS), its corpus beside it
# (<side>/fuzz/corpus/<target>); this directory holds the harness (fz.h,
# replay_main.c, seeds.c).
#
#   make fuzz           build/fuzz/<target> for every <side>/fuzz/fz_<target>.c:
#                       libFuzzer + ASan + UBSan, every object (a product's
#                       APP_EXTRA_SRC included) rebuilt with coverage instrumentation
#   make fuzz-smoke     each target for FUZZ_TIME seconds (default 30), from
#                       a scratch copy of its checked-in corpus; a crash leaves
#                       its input in build/fuzz-crashes/<target>/ and fails
#   make fuzz-nightly   the same with FUZZ_TIME=600
#   make fuzz-replay    every file of its corpus through each target
#                       once, under the ordinary ASan build (any compiler, no
#                       libFuzzer): the corpus as a regression test
#   make fuzz-merge     each scratch corpus minimised back into its corpus
#                       (inputs are renamed to their hashes, except a found
#                       bug's reproducer, <corpus>/repro-<what>,
#                       which is kept by name beside its unit test)
#   make fuzz-seeds     the seed writers (fuzz/seeds.c, the platform's targets'; and
#                       a product's own, APP_FUZZ_SEEDS) write the seed inputs into
#                       each target's corpus (made from the real encoders and the
#                       tests' recorded fixtures)
#
# libFuzzer needs clang with -fsanitize=fuzzer. Apple's clang lacks it, so on
# macOS FUZZ_CC is Homebrew LLVM's; on Linux it is the clang on PATH.
#
# A target is one file; adding <side>/fuzz/fz_<name>.c adds the target to every
# rule above with no edit here. Its corpus is <side>/fuzz/corpus/<name>/,
# created if missing. Target names are unique across the sides.

ifeq ($(UNAME),Darwin)
  FUZZ_CC ?= $(firstword $(wildcard /opt/homebrew/opt/llvm/bin/clang /usr/local/opt/llvm/bin/clang) clang)
else
  FUZZ_CC ?= clang
endif
FUZZ_TIME ?= 30
FUZZ_JOBS ?= 1

FUZZ_SAN := -fsanitize=address,undefined,fuzzer-no-link -fno-sanitize-recover=undefined -fno-omit-frame-pointer
APP_EXTRA_FUZZ_CFLAGS ?= $(filter-out -O2 -O3 -flto,$(APP_EXTRA_CFLAGS)) -O1 -g $(FUZZ_SAN)
FUZZ_CB_CFLAGS = $(filter-out -O2,$(CB_CFLAGS)) -O1 $(FUZZ_SAN) -I$(CB)fuzz
# The replay build (no libFuzzer): the asan variant's objects, the harness too.
FUZZ_REPLAY_CFLAGS = $(ASAN_CB_CFLAGS) -I$(CB)fuzz

$(eval $(call C_VARIANT,build/obj/fuzz,FUZZ_CC,FUZZ_CB_CFLAGS,,$(SIDE_STAMP)))
$(eval $(call C_VARIANT,build/obj/fuzz-extra,FUZZ_CC,APP_EXTRA_FUZZ_CFLAGS))
$(eval $(call C_VARIANT,build/obj/fuzz-replay,CC,FUZZ_REPLAY_CFLAGS,,$(SIDE_STAMP)))

FUZZ_ROOTS   := $(CB)core/fuzz $(addprefix $(CB)providers/,$(addsuffix /fuzz,$(CBASE_PROVIDERS))) $(APP_FUZZ_ROOTS)
FUZZ_SRC     := $(sort $(foreach r,$(FUZZ_ROOTS),$(wildcard $(r)/fz_*.c)))
FUZZ_ALL     := $(sort $(patsubst fz_%.c,%,$(notdir $(FUZZ_SRC))))
FUZZ_TARGETS := $(FUZZ_ALL)
ifneq ($(words $(FUZZ_ALL)),$(words $(FUZZ_SRC)))
  $(error two fuzz roots hold a target of the same name)
endif
# A target's file and its corpus, and every target as <name>=<corpus> for the
# shell loops below.
fz_src    = $(filter %/fz_$(1).c,$(FUZZ_SRC))
fz_corpus = $(patsubst %/fz_$(1).c,%/corpus/$(1),$(call fz_src,$(1)))
FUZZ_PAIRS = $(foreach t,$(FUZZ_TARGETS),$(t)=$(call fz_corpus,$(t)))
# What a target links beside its own file: fuzz/lib_*.c (the harness helpers).
FUZZ_LIB     := $(sort $(wildcard $(CB)fuzz/lib_*.c))
# Program sources a target replaces with its own (FUZZ_EXCLUDE_<target>, which
# a product sets for its own targets: one that makes the process's randomness a
# function of the input leaves out $(CB)core/src/util/random.c).
fuzz_lib = $(filter-out $(FUZZ_EXCLUDE_$(1)),$(LIB_SRC))
# Sources a target links beside the program's (FUZZ_EXTRA_<target>): the anonymous socket's target
# talks to a real server over loopback with the tests' client.
FUZZ_EXTRA_rt_anon := $(CB)core/test/lib_client.c

define FUZZ_PROG
build/fuzz/$(1): $$(call c_objs,build/obj/fuzz,$$(call fz_src,$(1)) $$(FUZZ_LIB) $$(FUZZ_EXTRA_$(1)) $$(call fuzz_lib,$(1))) $$(call c_objs,build/obj/fuzz-extra,$$(APP_EXTRA_SRC))
	@mkdir -p $$(@D)
	@echo "  LINK $$@"
	@$$(FUZZ_CC) $$(FUZZ_CB_CFLAGS) -fsanitize=fuzzer $$^ -o $$@ $$(LDLIBS)
build/asan/fuzz/$(1): $$(call c_objs,build/obj/fuzz-replay,$$(call fz_src,$(1)) $(CB)fuzz/replay_main.c $$(FUZZ_LIB) $$(FUZZ_EXTRA_$(1))) $$(call c_objs,build/obj/asan,$$(call fuzz_lib,$(1))) $$(call c_objs,build/obj/asan-extra,$$(APP_EXTRA_SRC))
	@mkdir -p $$(@D)
	@echo "  LINK $$@"
	@$$(CC) $$(ASAN_CB_CFLAGS) $$^ -o $$@ $$(LDLIBS)
endef
$(foreach t,$(FUZZ_TARGETS),$(eval $(call FUZZ_PROG,$(t))))

# The seed writers: ordinary programs under the asan build, the platform's
# (fuzz/seeds.c, for its own targets) and the product's (APP_FUZZ_SEEDS), each
# called with the staging directory. FUZZ_SEEDS_PROG is every one of them.
build/asan/fuzz/seeds: $(call c_objs,build/obj/fuzz-replay,$(CB)fuzz/seeds.c $(FUZZ_LIB)) $(call c_objs,build/obj/asan,$(LIB_SRC)) $(call c_objs,build/obj/asan-extra,$(APP_EXTRA_SRC))
	@mkdir -p $(@D)
	@echo "  LINK $@"
	@$(CC) $(ASAN_CB_CFLAGS) $^ -o $@ $(LDLIBS)
ifneq ($(strip $(APP_FUZZ_SEEDS)),)
build/asan/fuzz/app-seeds: $(call c_objs,build/obj/fuzz-replay,$(APP_FUZZ_SEEDS) $(FUZZ_LIB)) $(call c_objs,build/obj/asan,$(LIB_SRC)) $(call c_objs,build/obj/asan-extra,$(APP_EXTRA_SRC))
	@mkdir -p $(@D)
	@echo "  LINK $@"
	@$(CC) $(ASAN_CB_CFLAGS) $^ -o $@ $(LDLIBS)
endif

.PHONY: fuzz fuzz-smoke fuzz-nightly fuzz-replay fuzz-merge fuzz-seeds
fuzz: $(addprefix build/fuzz/,$(FUZZ_TARGETS))

# libFuzzer's flags for every run: no leak report on macOS (LeakSanitizer does
# not run there), a 25 s per-input timeout, a 2 GB RSS cap.
FUZZ_FLAGS ?= -timeout=25 -rss_limit_mb=2048 -print_final_stats=1
# One target at a time; each run's new inputs go to a scratch corpus so the
# checked-in one changes only through fuzz-merge.
define FUZZ_RUN
	@fail=0; for tc in $(FUZZ_PAIRS); do t=$${tc%%=*}; c=$${tc#*=}; \
	  mkdir -p build/fuzz-work/$$t build/fuzz-crashes/$$t $$c; \
	  cp -n $$c/* build/fuzz-work/$$t/ 2>/dev/null; \
	  echo "== fuzz $$t ($(1) s)"; \
	  build/fuzz/$$t $(FUZZ_FLAGS) -max_total_time=$(1) -artifact_prefix=build/fuzz-crashes/$$t/ \
	    $(if $(filter-out 1,$(FUZZ_JOBS)),-fork=$(FUZZ_JOBS) -ignore_crashes=0,) \
	    build/fuzz-work/$$t $$c > build/fuzz-work/$$t.log 2>&1 || { fail=1; echo "FUZZ FAILED: $$t"; tail -60 build/fuzz-work/$$t.log; }; \
	  grep -E '^stat::number_of_executed_units|^#[0-9]+.*DONE' build/fuzz-work/$$t.log | tail -1; \
	done; if [ $$fail -ne 0 ]; then exit 1; fi
endef
fuzz-smoke: fuzz
	$(call FUZZ_RUN,$(FUZZ_TIME))
fuzz-nightly: fuzz
	$(call FUZZ_RUN,600)

fuzz-replay: $(addprefix build/asan/fuzz/,$(FUZZ_TARGETS))
	@fail=0; for tc in $(FUZZ_PAIRS); do t=$${tc%%=*}; c=$${tc#*=}; \
	  mkdir -p $$c; printf '%-20s ' $$t; build/asan/fuzz/$$t $$c || fail=1; \
	done; if [ $$fail -ne 0 ]; then echo "FUZZ REPLAY FAILED"; exit 1; fi

fuzz-merge: fuzz
	@for tc in $(FUZZ_PAIRS); do t=$${tc%%=*}; c=$${tc#*=}; \
	  [ -d build/fuzz-work/$$t ] || continue; \
	  rm -rf build/fuzz-merged/$$t; mkdir -p build/fuzz-merged/$$t; \
	  build/fuzz/$$t -merge=1 $(FUZZ_FLAGS) build/fuzz-merged/$$t $$c build/fuzz-work/$$t > build/fuzz-work/$$t.merge.log 2>&1 \
	    && { cp $$c/repro-* build/fuzz-merged/$$t/ 2>/dev/null; true; } \
	    && rm -rf $$c && mv build/fuzz-merged/$$t $$c \
	    && echo "$$t: $$(ls $$c | wc -l | tr -d ' ') inputs"; \
	done

# The seeds land in build/fuzz-seeds/<target>, then in each target's corpus;
# seeds for a name that is no target are refused before any corpus is
# written. The program and the staging directory are variables so
# fuzz/test/fuzz_mk_test.sh can run these rules with a stand-in program.
FUZZ_SEEDS_PROG := build/asan/fuzz/seeds $(if $(strip $(APP_FUZZ_SEEDS)),build/asan/fuzz/app-seeds)
FUZZ_SEEDS_OUT  := build/fuzz-seeds
fuzz-seeds: $(FUZZ_SEEDS_PROG)
	rm -rf $(FUZZ_SEEDS_OUT) && mkdir -p $(FUZZ_SEEDS_OUT)
	for p in $(FUZZ_SEEDS_PROG); do $$p $(FUZZ_SEEDS_OUT) || exit 1; done
	@for d in $(FUZZ_SEEDS_OUT)/*; do [ -d "$$d" ] || continue; case " $(FUZZ_ALL) " in *" $${d##*/} "*) ;; \
	  *) echo "fuzz-seeds: seeds for $${d##*/}, which is no target"; exit 1;; esac; done
	@for tc in $(FUZZ_PAIRS); do t=$${tc%%=*}; c=$${tc#*=}; \
	  [ -d $(FUZZ_SEEDS_OUT)/$$t ] || continue; mkdir -p $$c; cp $(FUZZ_SEEDS_OUT)/$$t/* $$c/; \
	done

# Every target's name, for CI's matrix (ci.yml, fuzz_nightly.yml).
.PHONY: print-fuzz-targets
print-fuzz-targets:
	@echo $(FUZZ_ALL)
