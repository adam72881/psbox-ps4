# SPDX-License-Identifier: GPL-3.0-only
# Parámetros adaptados de los ejemplos hello_world/input de OpenOrbis v0.5.4.
ifeq ($(strip $(OO_PS4_TOOLCHAIN)),)
$(error Falta OO_PS4_TOOLCHAIN. Ejecuta source ~/.config/xcloud4/env.sh)
endif

SDK := $(OO_PS4_TOOLCHAIN)
X4_WEBRTC_ROOT ?= $(HOME)/.local/share/xcloud4/webrtc
X4_WEBRTC_PREFIX ?= $(X4_WEBRTC_ROOT)/prefix
CC := clang
CXX := clang++
LD := ld.lld
CFLAGS := --target=x86_64-pc-freebsd12-elf -std=c23 -fPIC -funwind-tables -O2 -g -Wall -Wextra -MMD -MP -c -isysroot $(SDK) -I$(X4_WEBRTC_ROOT)/overlay -I$(X4_WEBRTC_PREFIX)/include -isystem $(SDK)/include -D_BSD_SOURCE -DX4_OPENORBIS
# Allocation preparation only. Native decoder/SDP/request capabilities remain
# at the verified 720p configuration until a separate compatibility probe.
X4_VIDEO_CAPACITY_1080 ?= 0
ifneq ($(filter $(X4_VIDEO_CAPACITY_1080),0 1),$(X4_VIDEO_CAPACITY_1080))
$(error X4_VIDEO_CAPACITY_1080 must be 0 or 1)
endif
CFLAGS += -DX4_VIDEO_CAPACITY_1080=$(X4_VIDEO_CAPACITY_1080)
# Local source probe only; ordinary streaming defaults remain unchanged.
X4_VIDEO_PROBE ?= 0
ifneq ($(filter $(X4_VIDEO_PROBE),0 1),$(X4_VIDEO_PROBE))
$(error X4_VIDEO_PROBE must be 0 or 1)
endif
CFLAGS += -DX4_VIDEO_PROBE=$(X4_VIDEO_PROBE)
# Independently selected streaming request/pipeline, never inferred from a
# package filename. Profile 0 preserves the legacy/probe compilation path.
X4_STREAM_PROFILE ?= 0
ifneq ($(words $(X4_STREAM_PROFILE)),1)
$(error X4_STREAM_PROFILE must be one literal value: 0, 1 or 2)
endif
ifneq ($(filter $(X4_STREAM_PROFILE),0 1 2),$(X4_STREAM_PROFILE))
$(error X4_STREAM_PROFILE must be 0, 1 or 2)
endif
ifeq ($(X4_VIDEO_PROBE),1)
ifneq ($(X4_STREAM_PROFILE),0)
$(error The local probe requires X4_STREAM_PROFILE=0)
endif
endif
CFLAGS += -DX4_STREAM_PROFILE=$(X4_STREAM_PROFILE)
# Instrumented try-gate baseline for the owner-requested A/B/A trial.
X4_INGRESS_MODE ?= mpsc
ifeq ($(X4_INGRESS_MODE),baseline)
CFLAGS += -DX4_INGRESS_BASELINE
else ifneq ($(X4_INGRESS_MODE),mpsc)
$(error X4_INGRESS_MODE must be mpsc or baseline)
endif
X4_RECEIVER_NACK ?= 0
ifneq ($(filter $(X4_RECEIVER_NACK),0 1),$(X4_RECEIVER_NACK))
$(error X4_RECEIVER_NACK must be 0 or 1)
endif
CFLAGS += -DX4_RECEIVER_NACK=$(X4_RECEIVER_NACK)
X4_NACK_GRACE_MS ?= 0
ifneq ($(filter $(X4_NACK_GRACE_MS),0 45),$(X4_NACK_GRACE_MS))
$(error X4_NACK_GRACE_MS must be 0 or 45)
endif
ifeq ($(X4_NACK_GRACE_MS),45)
ifneq ($(X4_RECEIVER_NACK),1)
$(error Grace45 requires the existing receiver NACK generator ON)
endif
endif
X4_RUMBLE ?= 1
ifneq ($(filter $(X4_RUMBLE),0 1),$(X4_RUMBLE))
$(error X4_RUMBLE must be 0 or 1)
endif
CFLAGS += -DX4_NACK_GRACE_MS=$(X4_NACK_GRACE_MS) -DX4_RUMBLE=$(X4_RUMBLE)
X4_PUBLIC ?= 0
ifneq ($(filter $(X4_PUBLIC),0 1),$(X4_PUBLIC))
$(error X4_PUBLIC must be 0 or 1)
endif
ifeq ($(X4_VIDEO_PROBE)_$(X4_PUBLIC),1_1)
$(error The local probe requires X4_PUBLIC=0)
endif
CFLAGS += -DX4_PUBLIC=$(X4_PUBLIC)
# Same-build evidence for the probe and explicitly selected streaming pipeline.
# This must precede CXXFLAGS so both languages emit same-build .su evidence.
ifneq ($(filter 1,$(X4_VIDEO_PROBE))$(filter 1 2,$(X4_STREAM_PROFILE)),)
CFLAGS += -fstack-usage -Werror=vla -Werror=alloca
endif
CXXFLAGS := $(filter-out -std=c23,$(CFLAGS)) -std=c++17 -nostdinc++ -isystem $(SDK)/include/c++/v1
LDFLAGS := -m elf_x86_64 -pie --script $(SDK)/link.x --eh-frame-hdr -L$(SDK)/lib -Map=build/xcloud4.map
RTC_LIBS := $(X4_WEBRTC_ROOT)/build-datachannel/libdatachannel-static.a \
    $(X4_WEBRTC_ROOT)/build-datachannel/deps/libjuice/libjuice-static.a \
    $(X4_WEBRTC_ROOT)/build-datachannel/deps/usrsctp/usrsctplib/libusrsctp.a \
    $(X4_WEBRTC_ROOT)/build-datachannel/deps/libsrtp/libsrtp2.a \
    $(X4_WEBRTC_PREFIX)/lib/libmbedtls.a $(X4_WEBRTC_PREFIX)/lib/libmbedx509.a \
    $(X4_WEBRTC_PREFIX)/lib/libmbedcrypto.a $(X4_WEBRTC_PREFIX)/lib/libeverest.a \
    $(X4_WEBRTC_PREFIX)/lib/libp256m.a $(X4_WEBRTC_PREFIX)/lib/libopus.a
LIBS := -lScePosix -lc -lkernel -lSceVideoOut -lSceGnmDriver -lScePad -lSceUserService -lSceSystemService -lc++ -lc++abi -lunwind -lm
SOURCES := $(wildcard src/core/*.c src/ui/*.c src/video/*.c src/input/*.c src/audio/*.c src/auth/*.c src/streaming/*.c src/media/*.c)
# Rebuild this complete LLVM 11 object with native ETIMEDOUT=60. The SDK's
# precompiled object compares against 110. Explicit objects precede -lc++;
# providing all five strong methods keeps that archive member unselected.
# llvmorg-11.0.0, commit 176249bd6732a8044d457092ed932768724a6f06.
# Exact upstream SHA256: 39d39fc245662b760c7c74a29218eeb8c66805996dc00e25e1e77693e53a77a5.
# Full license: docs/licenses/LLVM-libcxx-11-LICENSE.TXT.
CXX_SOURCES := src/streaming/rtc_condition_variable.cpp
OBJECTS := $(patsubst src/%.c,build/%.o,$(SOURCES)) $(patsubst src/%.cpp,build/%.o,$(CXX_SOURCES))
OBJECTS += build/third_party/qrcodegen/qrcodegen.o

.PHONY: all package
all: build/eboot.bin
package: build/eboot.bin
	X4_STREAM_PROFILE=$(X4_STREAM_PROFILE) X4_PUBLIC=$(X4_PUBLIC) bash scripts/empaquetar.sh

build:
	mkdir -p build

# One stable content stamp for every functional build flag. The forced recipe
# checks content on each invocation, but unchanged content keeps its mtime so
# a no-change build does not rebuild objects. All objects depend on it because
# these flags are supplied globally to C/C++ translation units.
.PHONY: build-config-force
build-config-force:
build/build_config.h: build-config-force | build
	@printf '#define X4_RECEIVER_NACK %s\n#define X4_NACK_GRACE_MS %s\n#define X4_RUMBLE %s\n#define X4_INGRESS_MODE_BASELINE %s\n#define X4_VIDEO_CAPACITY_1080 %s\n#define X4_VIDEO_PROBE %s\n#define X4_STREAM_PROFILE %s\n#define X4_PUBLIC %s\n' '$(X4_RECEIVER_NACK)' '$(X4_NACK_GRACE_MS)' '$(X4_RUMBLE)' '$(if $(filter baseline,$(X4_INGRESS_MODE)),1,0)' '$(X4_VIDEO_CAPACITY_1080)' '$(X4_VIDEO_PROBE)' '$(X4_STREAM_PROFILE)' '$(X4_PUBLIC)' > $@.tmp
	@if test -f $@ && cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi
$(OBJECTS): build/build_config.h

build/%.o: src/%.c Makefile
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

build/third_party/%.o: third_party/%.c Makefile
	mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -o $@ $<

build/streaming/rtc_condition_variable.o: CXXFLAGS += -D_LIBCPP_BUILDING_LIBRARY

build/%.o: src/%.cpp Makefile
	mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -o $@ $<

build/xcloud4.elf: $(OBJECTS) $(RTC_LIBS)
	$(LD) $(SDK)/lib/crt1.o $(OBJECTS) -o $@ $(LDFLAGS) --start-group $(RTC_LIBS) $(LIBS) --end-group

# Audit the actual linked probe before SELF; never weaken the normal build.
ifeq ($(X4_VIDEO_PROBE),1)
.PHONY: probe-posix-audit probe-stack-audit
probe-posix-audit: build/xcloud4.elf scripts/audit_probe_posix_elf.py
	python3 scripts/audit_probe_posix_elf.py build/xcloud4.elf --objdump objdump --output build/probe-posix-audit.json
# Probe workers request explicit 256 KiB stacks. The gate checks measured
# frames and known paths, with separately reported 16 KiB opaque allowances;
# the main stack is not guessed and console high-water is separate evidence.
# A phony prerequisite rechecks the actual ELF on every SELF/package attempt;
# an old SELF or an earlier JSON PASS cannot bypass a failed current audit.
probe-stack-audit: build/xcloud4.elf scripts/audit_probe_stack.py scripts/prepare_probe_stack_policy.py
	python3 scripts/prepare_probe_stack_policy.py --elf build/xcloud4.elf --su-root build --output build/probe-stack-policy.json
	python3 scripts/audit_probe_stack.py --elf build/xcloud4.elf --policy build/probe-stack-policy.json --objdump objdump --output build/probe-stack-audit.json
build/eboot.bin: probe-posix-audit probe-stack-audit
else ifneq ($(X4_STREAM_PROFILE),0)
.PHONY: stream-posix-audit stream-stack-audit
stream-posix-audit: build/xcloud4.elf scripts/audit_stream_posix_elf.py scripts/audit_probe_posix_elf.py
	python3 scripts/audit_stream_posix_elf.py build/xcloud4.elf --objdump objdump --output build/stream-posix-audit.json
stream-stack-audit: build/xcloud4.elf scripts/prepare_stream_stack_policy.py scripts/audit_probe_stack.py
	python3 scripts/prepare_stream_stack_policy.py --elf build/xcloud4.elf --su-root build --output build/stream-stack-policy.json
	python3 scripts/audit_probe_stack.py --elf build/xcloud4.elf --policy build/stream-stack-policy.json --objdump objdump --output build/stream-stack-audit.json
build/eboot.bin: stream-posix-audit stream-stack-audit
endif

build/eboot.bin: build/xcloud4.elf
	$(SDK)/bin/linux/create-fself -in=build/xcloud4.elf -out=build/xcloud4.oelf --eboot $@ --paid 0x3800000000000011

-include $(OBJECTS:.o=.d)
