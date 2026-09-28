# Which clang compiles a kernel for wasm32, and the guard that refuses the
# wrong one. Included by a product Makefile in its wasm section (NOT above its
# first target: this file defines the `wasm-cc-check` target, and the first
# target make reads is the default goal).
#
#   include ../../shared/tools/wasm_cc.mk
#   wasm: wasm-cc-check
#
# THE COMPILER IS PART OF THE OUTPUT, so picking it up off PATH is picking up
# whatever is there. On a Mac that is Apple clang, and the old protection was
# that Apple clang could not target wasm32 at all, so the wrong one failed
# loudly. That protection expired: Apple clang 21 targets wasm32 fine and
# produces different bytes from the pinned toolchain, silently (measured on
# the first product to ship a module: 271 B larger; on every freestanding
# object of the newer kernels, every single .o differs). The build succeeds and
# the bytes are wrong, which is why this is a guard and not advice.
#
# So macOS defaults to the pinned Homebrew LLVM when it is installed, and
# wasm-cc-check refuses Apple clang outright. Setting WASM_CC= explicitly still
# wins (CI's shared/scripts/ci_llvm.sh exports it), and still meets the guard.
ifeq ($(origin WASM_CC),undefined)
  ifneq ($(wildcard /opt/homebrew/opt/llvm/bin/clang),)
    WASM_CC := /opt/homebrew/opt/llvm/bin/clang
  else
    WASM_CC := clang
  endif
endif

# The pinned major, matching shared/scripts/ci_llvm.sh LLVM_VERSION. A warning
# and not a refusal: a different point release is a few bytes, a different
# MAJOR is kilobytes, and both are worth saying out loud without stopping
# someone who knows why they are doing it.
WASM_LLVM_MAJOR ?= 22

.PHONY: wasm-cc-check
wasm-cc-check:
	@v="$$($(WASM_CC) --version 2>/dev/null | head -1)"; \
	case "$$v" in \
	  "") echo "wasm: WASM_CC=$(WASM_CC) is not runnable" >&2; exit 1;; \
	  *"Apple clang"*) \
	    echo "wasm: WASM_CC=$(WASM_CC) is $$v" >&2; \
	    echo "wasm: Apple clang targets wasm32 but does NOT produce the pinned bytes" >&2; \
	    echo "wasm: install the pinned toolchain (brew install llvm binaryen) or pass WASM_CC=" >&2; \
	    exit 1;; \
	esac; \
	case "$$v" in \
	  *"clang version $(WASM_LLVM_MAJOR)."*) ;; \
	  *) echo "wasm: WASM_CC is $$v, not the pinned clang $(WASM_LLVM_MAJOR) - sizes will differ from CI" >&2;; \
	esac
