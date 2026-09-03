include NickelHook/NickelHook.mk

override LIBRARY  := libnfolders.so
override SOURCES  += nfolders.cc
override CFLAGS   += -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden
override CXXFLAGS += -Wall -Wextra -Werror -Wno-missing-field-initializers -fvisibility=hidden -fvisibility-inlines-hidden
# `test` isn't an ARM build target, so it must be excluded before the second
# include below runs NickelHook's configure step -- placed after, `make test`
# would try to configure the cross toolchain and fail with a confusing error.
override SKIPCONFIGURE += test

include NickelHook/NickelHook.mk

# Host test build. Separate from NickelHook's cross build on purpose: these
# compile the PURE sources (nffmt, nflist) with the system compiler and host
# Qt5, which is the only way anything here runs off-device.
#
# The host Qt is 5.15 and the device's is 5.2.1. Every API the pure sources use
# exists in both, and reaching for a 5.15-only call breaks `./nickeltc make`
# on the next device build rather than failing quietly on the panel -- which is
# why the skew is acceptable rather than merely tolerated.
HOST_CXX      ?= g++
HOST_PURE     := nffmt.cc nflist.cc
HOST_TESTSRC  := $(wildcard tests/test_*.cc)
HOST_TESTBIN  := $(patsubst tests/%.cc,build/%,$(HOST_TESTSRC))
# Header prerequisites are load-bearing, not tidiness: without them a test
# binary is not relinked when only a header changed, so a deliberately broken
# guard in a header runs against a stale binary and appears not to fail --
# which would silently invalidate the one discipline this rung leans on.
HOST_HDR      := $(wildcard *.h) $(wildcard tests/*.h)
HOST_CXXFLAGS := -std=gnu++11 -O1 -g -Wall -Wextra -Werror -I. -Itests \
                 $(shell pkg-config --cflags Qt5Core)
HOST_LDLIBS   := $(shell pkg-config --libs Qt5Core)

build:
	@mkdir -p build

build/test_%: tests/test_%.cc $(HOST_PURE) $(HOST_HDR) | build
	$(HOST_CXX) $(HOST_CXXFLAGS) -o $@ $< $(HOST_PURE) $(HOST_LDLIBS)

.PHONY: test
test: $(HOST_TESTBIN)
	@rc=0; for t in $(HOST_TESTBIN); do echo "== $$t"; ./$$t || rc=1; done; exit $$rc
