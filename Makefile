# ODFileSystem — Amiga optical filesystem
# SPDX-License-Identifier: BSD-2-Clause
#
# Makefile for host and Amiga cross-compilation builds

# ---- toolchain selection ----

# Amiga cross-compiler.
# Override with CC=ppc-amigaos-gcc for an AmigaOS 4 PPC build.
ifeq ($(origin CC),default)
CC      = m68k-amigaos-gcc
endif

# AROS cross-compiler (override: make CC=m68k-aros-gcc AROS=1)
# When AROS=1, uses -static instead of -noixemul and defines __AROS__
AROS ?= 0

# Derive target tools from CC so CC=ppc-amigaos-gcc also selects the
# matching ppc-amigaos-ar/strip/size tools.
AMIGA_CC_TARGET   := $(shell $(CC) -dumpmachine 2>/dev/null)
AMIGA_TOOL_PREFIX ?= $(patsubst %-gcc,%,$(notdir $(CC)))
AMIGA_AR          ?= $(AMIGA_TOOL_PREFIX)-ar
AMIGA_SIZE        ?= $(AMIGA_TOOL_PREFIX)-size
AMIGA_OBJCOPY     ?= $(AMIGA_TOOL_PREFIX)-objcopy
STRIP             ?= $(AMIGA_TOOL_PREFIX)-strip

ifneq ($(filter ppc-amigaos,$(AMIGA_CC_TARGET)),)
AMIGA_TARGET ?= os4
else ifeq ($(AROS),1)
AMIGA_TARGET ?= aros
else
AMIGA_TARGET ?= os3
endif

ifeq ($(AMIGA_TARGET),os4)
AMIGA_OSDIR := os4
else
AMIGA_OSDIR := os3
endif

# NDK include path (override with: make NDK_PATH=/your/path)
ifeq ($(AMIGA_TARGET),os4)
NDK_PATH ?= $(shell realpath $$(dirname $$(which $(CC)))/../ppc-amigaos/SDK/include/include_h 2>/dev/null)
else
NDK_PATH ?= $(shell realpath $$(dirname $$(which $(CC)))/../m68k-amigaos/ndk-include 2>/dev/null)
endif

# Host compiler
HOSTCC ?= cc

# ---- common flags ----

AMIGA_DATE ?= $(shell date '+%-d.%-m.%Y')
ODFS_GIT_VERSION ?= $(shell desc=$$(git describe --tags --match "v*" --dirty --always 2>/dev/null || echo unknown); printf '%s\n' "$$desc" | grep -q '^v' && printf '%s' "$$desc" || printf 'early-0-g%s' "$$desc")

INCLUDES = -I include -I backends
AMIGA_PLATFORM_INCLUDES = -I platform/amiga \
                          -I platform/amiga/common \
                          -I platform/amiga/$(AMIGA_OSDIR)
AMIGA_INCLUDES = $(INCLUDES) $(AMIGA_PLATFORM_INCLUDES) \
                 $(if $(NDK_PATH),-I$(NDK_PATH))

# ---- optional 3rdparty submodules ----

GIT := $(shell git -C "$(CURDIR)" rev-parse --git-dir 1>/dev/null 2>&1 \
	&& command -v git)
ifneq ($(GIT),)
freshsubs := $(shell git submodule update --init 3rdparty/libcodesets \
	2>/dev/null)
endif

# ---- host build flags ----

HOSTCFLAGS  = -std=c11 -O2 -g \
              -Wall -Wextra -Wpedantic -Werror \
              -Wshadow -Wstrict-prototypes -Wmissing-prototypes \
              -Wno-unused-parameter \
              -MMD -MP
HOSTLDFLAGS =

# ---- Amiga build options ----

# Optimization level. Speed is the release priority; the ROM profile
# overrides this with -Os to stay within its size budget.
AMIGA_OPT ?= -O2

# Serial debug output (override with: make SERIAL_DEBUG=1)
SERIAL_DEBUG ?= 0

# Packet trace instrumentation (override with: make PACKET_TRACE=1)
PACKET_TRACE ?= 0

# Release size limits (override when intentional growth is approved)
ifeq ($(AMIGA_TARGET),os4)
AMIGA_SIZE_LIMIT ?= 147456
else
AMIGA_SIZE_LIMIT ?= 98304
endif
ROM_SIZE_LIMIT   ?= 32768
SIZE_LIMIT_NAME  ?= AMIGA_SIZE_LIMIT
SIZE_LIMIT_DESC  ?= release Amiga handler

# Backend selection (override to disable: make FEATURE_UDF=0)
FEATURE_ISO9660      ?= 1
FEATURE_ROCK_RIDGE   ?= 1
FEATURE_JOLIET       ?= 1
FEATURE_MULTISESSION ?= 1
FEATURE_UDF          ?= 1
FEATURE_HFS          ?= 1
FEATURE_HFSPLUS      ?= 1
FEATURE_CDDA         ?= 1

# ---- Amiga build flags (following xsysinfo conventions) ----

FEATURE_DEFS = \
          -DODFS_AMIGA_DATE=\"$(AMIGA_DATE)\" \
          -DODFS_GIT_VERSION=\"$(ODFS_GIT_VERSION)\" \
          -DODFS_SERIAL_DEBUG=$(SERIAL_DEBUG) \
          -DODFS_PACKET_TRACE=$(PACKET_TRACE) \
          -DODFS_FEATURE_LOG=$(SERIAL_DEBUG) \
          -DODFS_FEATURE_ISO9660=$(FEATURE_ISO9660) \
          -DODFS_FEATURE_ROCK_RIDGE=$(FEATURE_ROCK_RIDGE) \
          -DODFS_FEATURE_JOLIET=$(FEATURE_JOLIET) \
          -DODFS_FEATURE_MULTISESSION=$(FEATURE_MULTISESSION) \
          -DODFS_FEATURE_UDF=$(FEATURE_UDF) \
          -DODFS_FEATURE_HFS=$(FEATURE_HFS) \
          -DODFS_FEATURE_HFSPLUS=$(FEATURE_HFSPLUS) \
          -DODFS_FEATURE_CDDA=$(FEATURE_CDDA)

ifeq ($(AMIGA_TARGET),aros)
AMIGA_CPUFLAGS ?= -m68000 -mtune=68020-60 -msoft-float
AMIGA_SYSFLAGS ?= -static
AMIGA_WARNFLAGS =
AMIGA_DEFS     = -DAMIGA -D__AROS__
LDFLAGS        = $(AMIGA_SYSFLAGS) $(LTO)
LIBS           = -lamiga -lgcc
HANDLER_LDFLAGS = -nostartfiles
HANDLER_LIBS   = -nostdlib -Wl,-u,_exit -lgcc -lc -lgcc -lamiga -ramiga-dev
else ifeq ($(AMIGA_TARGET),os4)
AMIGA_CRT      ?= newlib
AMIGA_CPUFLAGS ?= -mcpu=powerpc
# No unwind tables: the freestanding handler has no exception support,
# and the kickstart loader expects plain PT_LOAD program headers only.
AMIGA_SYSFLAGS ?= -mcrt=$(AMIGA_CRT) -fno-asynchronous-unwind-tables
AMIGA_WARNFLAGS =
AMIGA_DEFS     = -DAMIGA -D__USE_INLINE__ -D__USE_BASETYPE__
LDFLAGS        = $(AMIGA_SYSFLAGS) $(LTO)
# Keep OS4 library/interface ownership explicit in os4/sys_compat.c.
# Do not add -lauto to the handler link.
LIBS           = -lc -lgcc
# The handler must not run the newlib C runtime startup: it consumes
# the first process message, which is the handler's ACTION_STARTUP
# packet. os4/start.c provides the freestanding entry instead.
# -static keeps gcc from passing --eh-frame-hdr, so the binary carries
# only the plain PT_LOAD program headers the kickstart loader expects.
HANDLER_LDFLAGS = -nostartfiles -static -Wl,-u,_start
HANDLER_LIBS   = -nostdlib -lgcc
else
AMIGA_CPUFLAGS ?= -m68000 -mtune=68020-60 -msoft-float
AMIGA_SYSFLAGS ?= -noixemul
AMIGA_WARNFLAGS =
AMIGA_DEFS     = -DAMIGA
LDFLAGS        = $(AMIGA_SYSFLAGS) $(LTO)
LIBS           = -lamiga -lgcc
HANDLER_LDFLAGS = -nostartfiles
HANDLER_LIBS   = -nostdlib -Wl,-u,_exit -lgcc -lc -lgcc -lamiga -ramiga-dev
endif

CFLAGS = $(AMIGA_OPT) $(AMIGA_CPUFLAGS) $(AMIGA_SYSFLAGS) -nostartfiles \
         -Wall -Wextra -Werror \
         $(AMIGA_WARNFLAGS) \
         -Wstrict-prototypes -Wmissing-prototypes \
         -Wno-array-bounds \
         -MMD -MP \
         $(AMIGA_DEFS) $(FEATURE_DEFS) $(LTO)

# ---- build directories ----

HOST_BUILD  = build/host
AMIGA_BUILD = build/amiga
ROM_BUILD   = build/amiga-rom
AMIGA_TEST_BUILD = build/amiga-test
ROM_TEST_BUILD   = build/amiga-rom-test
AMIGA_020_BUILD  = build/amiga-020

# 68020 release variant: hardware mul/div and 020 addressing modes make
# the handler both faster and smaller than the 68000 build.
AMIGA_020_CPUFLAGS ?= -m68020 -msoft-float

# ---- shared source lists ----

# Core library (shared between host and Amiga)
CORE_SRCS = \
    core/error.c \
    core/log.c \
    core/node.c \
    core/namefix.c \
    core/cache_block.c \
    core/cache_meta.c \
    core/charset.c \
    core/ancestry.c \
    core/mount.c \
    core/session.c \
    backends/iso9660/iso9660.c \
    backends/rock_ridge/rock_ridge.c \
    backends/joliet/joliet.c \
    backends/udf/udf.c \
    backends/hfs/hfs.c \
    backends/hfsplus/hfsplus.c \
    backends/cdda/cdda.c

# Host-only sources
HOST_SRCS = platform/host/file_media.c

# Amiga handler sources
AMIGA_SRCS = platform/amiga/handler_main.c \
    platform/amiga/printf_local.c \
    platform/amiga/$(AMIGA_OSDIR)/sys_compat.c
ifeq ($(AMIGA_TARGET),os4)
AMIGA_SRCS += platform/amiga/os4/main.c \
    platform/amiga/os4/start.c \
    platform/amiga/os4/freestanding.c \
    platform/amiga/os4/vector_port.c
else
AMIGA_SRCS += platform/amiga/libc_stubs.c
endif

# Freestanding libc replacements: stop the compiler from recognizing
# the copy loops and emitting calls to the functions being defined.
$(AMIGA_BUILD)/platform/amiga/os4/freestanding.o: CFLAGS += -fno-builtin

# Amiga assembly
ifeq ($(AMIGA_TARGET),os4)
AMIGA_ASM_SRCS =
else
AMIGA_ASM_SRCS = platform/amiga/startup.S
endif
AMIGA_ASM_OBJS = $(patsubst %.S,$(AMIGA_BUILD)/%.o,$(AMIGA_ASM_SRCS))

HOST_LIB_SRCS  = $(CORE_SRCS) $(HOST_SRCS)
HOST_LIB_OBJS  = $(patsubst %.c,$(HOST_BUILD)/%.o,$(HOST_LIB_SRCS))
HOST_LIB_DEPS  = $(HOST_LIB_OBJS:.o=.d)

AMIGA_LIB_SRCS = $(CORE_SRCS) $(AMIGA_SRCS)
AMIGA_LIB_OBJS = $(patsubst %.c,$(AMIGA_BUILD)/%.o,$(AMIGA_LIB_SRCS))
AMIGA_LIB_DEPS = $(AMIGA_LIB_OBJS:.o=.d)
AMIGA_ASM_DEPS = $(AMIGA_ASM_OBJS:.o=.d)

# ---- test binaries (host only) ----

TEST_SRCS = $(wildcard tests/unit/test_*.c)
TEST_BINS = $(patsubst tests/unit/%.c,$(HOST_BUILD)/tests/%,$(TEST_SRCS))
TEST_DEPS = $(patsubst tests/unit/%.c,$(HOST_BUILD)/tests/%.d,$(TEST_SRCS))

# ---- fuzz binaries (host only) ----

FUZZ_SRCS = $(wildcard tests/fuzz/fuzz_*.c)
FUZZ_BINS = $(patsubst tests/fuzz/%.c,$(HOST_BUILD)/tests/%,$(FUZZ_SRCS))
FUZZ_DEPS = $(patsubst tests/fuzz/%.c,$(HOST_BUILD)/tests/%.d,$(FUZZ_SRCS))

# ---- host tool binaries ----

TOOL_NAMES = imginfo imgls imgcat imgstat imgbench imgdump
TOOL_BINS  = $(patsubst %,$(HOST_BUILD)/tools/%,$(TOOL_NAMES))
TOOL_DEPS  = $(patsubst %,$(HOST_BUILD)/tools/%.d,$(TOOL_NAMES))

# ---- handler target (Amiga) ----

HANDLER      = $(AMIGA_BUILD)/ODFileSystem
HANDLER_020  = $(AMIGA_020_BUILD)/ODFileSystem
KICKSTART_MODULE =
AMIGA_ARTIFACTS = $(HANDLER)
ifeq ($(AMIGA_TARGET),os4)
KICKSTART_MODULE = $(AMIGA_BUILD)/CDFileSystem
AMIGA_ARTIFACTS += $(KICKSTART_MODULE)
endif
TEST_HANDLER = $(AMIGA_TEST_BUILD)/ODFileSystem
AMIGA_TEST_TOOL = $(AMIGA_TEST_BUILD)/test_handler
ADF          = $(AMIGA_TEST_BUILD)/ODFileSystem.adf
ADF_VOLUME   = ODFileSystem
ADF_DOSDRIVER      = platform/amiga/dosdrivers/CD0
ADF_DOSDRIVER_ICON = platform/amiga/dosdrivers/CD0.info
XDFTOOL      ?= xdftool
LHA          ?= lha
AMIGAOS3_PACKAGE_NAME ?= ODFileSystem
AMIGAOS3_PACKAGE     = $(AMIGA_BUILD)/$(AMIGAOS3_PACKAGE_NAME).lha
AMIGAOS3_PACKAGE_DIR = $(AMIGA_BUILD)/$(AMIGAOS3_PACKAGE_NAME)-lha
AMIGAOS3_README      = docs/ODFileSystem.readme
AMIGAOS4_PACKAGE_NAME ?= ODFileSystem-amigaos4
AMIGAOS4_PACKAGE     = $(AMIGA_BUILD)/$(AMIGAOS4_PACKAGE_NAME).lha
AMIGAOS4_PACKAGE_DIR = $(AMIGA_BUILD)/$(AMIGAOS4_PACKAGE_NAME)-lha
AMIGAOS4_README      = docs/ODFileSystem_OS4.readme

# ==================================================================
# targets
# ==================================================================

.PHONY: all host amiga amiga-test amiga-020 adf rom rom-test lib tests tools fuzz
.PHONY: amix amix-lib amix-tools amix-mount amix-kernel amix-kernel-module
.PHONY: check golden-check malformed-check fuzz-check integration-check
.PHONY: clean size amigaos3-lha amigaos4-lha

all: host

host: lib tests tools

amiga: $(AMIGA_ARTIFACTS)
	@echo "  $(HANDLER) built successfully"
	@size=$$(wc -c < "$(HANDLER)"); \
	echo "  Handler size: $$size bytes"; \
	if [ "$(ENFORCE_SIZE_LIMITS)" != "0" ] && [ "$$size" -gt "$(AMIGA_SIZE_LIMIT)" ]; then \
		echo "  ERROR: $(SIZE_LIMIT_DESC) exceeds $(AMIGA_SIZE_LIMIT) bytes"; \
		echo "  If this growth is intentional, rerun with $(SIZE_LIMIT_NAME)=<new-limit>"; \
		exit 1; \
	fi; \
	if [ -n "$(KICKSTART_MODULE)" ]; then \
		ksize=$$(wc -c < "$(KICKSTART_MODULE)"); \
		echo "  Kickstart module: $(KICKSTART_MODULE)"; \
		echo "  CDFileSystem size: $$ksize bytes"; \
	fi

amiga-test:
	@$(MAKE) --no-print-directory \
		AMIGA_BUILD=$(AMIGA_TEST_BUILD) \
		ENFORCE_SIZE_LIMITS=0 \
		SERIAL_DEBUG=1 \
		amiga

amiga-020:
	@if [ "$(AMIGA_TARGET)" != "os3" ]; then \
		echo "  ERROR: amiga-020 requires CC=m68k-amigaos-gcc"; \
		exit 1; \
	fi
	@$(MAKE) --no-print-directory \
		AMIGA_BUILD=$(AMIGA_020_BUILD) \
		AMIGA_CPUFLAGS="$(AMIGA_020_CPUFLAGS)" \
		SIZE_LIMIT_DESC="release Amiga 68020 handler" \
		amiga

adf: amiga-test amiga-020 $(AMIGA_TEST_TOOL) $(ADF_DOSDRIVER) $(ADF_DOSDRIVER_ICON) Makefile
	@mkdir -p $(dir $(ADF))
	@echo "  ADF   $(ADF)"
	@$(XDFTOOL) -f $(ADF) \
		create + \
		format "$(ADF_VOLUME)" + \
		makedir L + \
		makedir C + \
		write $(TEST_HANDLER) L + \
		write $(HANDLER_020) L/ODFileSystem020 + \
		write $(AMIGA_TEST_TOOL) C/test_handler + \
		write $(ADF_DOSDRIVER) + \
		write $(ADF_DOSDRIVER_ICON)
	@echo "  ADF image ready: $(ADF)"

amigaos3-lha:
	@if [ "$(AMIGA_TARGET)" != "os3" ]; then \
		echo "  ERROR: amigaos3-lha requires CC=m68k-amigaos-gcc"; \
		exit 1; \
	fi
	@$(MAKE) --no-print-directory AMIGA_BUILD=$(AMIGA_BUILD) amiga
	@$(MAKE) --no-print-directory AMIGA_020_BUILD=$(AMIGA_020_BUILD) \
		amiga-020
	@$(MAKE) --no-print-directory AMIGA_TEST_BUILD=$(AMIGA_TEST_BUILD) \
		amiga-test
	@$(MAKE) --no-print-directory ROM_BUILD=$(ROM_BUILD) rom
	@$(MAKE) --no-print-directory ROM_TEST_BUILD=$(ROM_TEST_BUILD) \
		rom-test
	@rm -rf "$(AMIGAOS3_PACKAGE_DIR)" "$(AMIGAOS3_PACKAGE)"
	@mkdir -p "$(AMIGAOS3_PACKAGE_DIR)"
	@cp "$(HANDLER)" "$(AMIGAOS3_PACKAGE_DIR)/ODFileSystem"
	@cp "$(HANDLER_020)" "$(AMIGAOS3_PACKAGE_DIR)/ODFileSystem020"
	@cp "$(TEST_HANDLER)" "$(AMIGAOS3_PACKAGE_DIR)/ODFileSystem-test"
	@cp "$(ROM_BUILD)/ODFileSystem" \
		"$(AMIGAOS3_PACKAGE_DIR)/ODFileSystem-rom"
	@cp "$(ROM_TEST_BUILD)/ODFileSystem" \
		"$(AMIGAOS3_PACKAGE_DIR)/ODFileSystem-rom-test"
	@cp "$(AMIGAOS3_README)" "$(AMIGAOS3_PACKAGE_DIR)/README.md"
	@echo "  LHA   $(AMIGAOS3_PACKAGE)"
	@(cd "$(AMIGAOS3_PACKAGE_DIR)" && \
		$(LHA) -aq "$(CURDIR)/$(AMIGAOS3_PACKAGE)" \
		ODFileSystem ODFileSystem020 ODFileSystem-test \
		ODFileSystem-rom ODFileSystem-rom-test README.md)
	@echo "  LHA archive ready: $(AMIGAOS3_PACKAGE)"

amigaos4-lha:
	@if [ "$(AMIGA_TARGET)" != "os4" ]; then \
		echo "  ERROR: amigaos4-lha requires CC=ppc-amigaos-gcc"; \
		exit 1; \
	fi
	@$(MAKE) --no-print-directory AMIGA_BUILD=$(AMIGA_BUILD) amiga
	@$(MAKE) --no-print-directory AMIGA_TEST_BUILD=$(AMIGA_TEST_BUILD) \
		amiga-test
	@rm -rf "$(AMIGAOS4_PACKAGE_DIR)" "$(AMIGAOS4_PACKAGE)"
	@mkdir -p "$(AMIGAOS4_PACKAGE_DIR)"
	@cp "$(HANDLER)" "$(AMIGAOS4_PACKAGE_DIR)/ODFileSystem-amigaos4"
	@cp "$(KICKSTART_MODULE)" "$(AMIGAOS4_PACKAGE_DIR)/CDFileSystem"
	@cp "$(TEST_HANDLER)" \
		"$(AMIGAOS4_PACKAGE_DIR)/ODFileSystem-amigaos4-test"
	@cp "$(AMIGA_TEST_BUILD)/CDFileSystem" \
		"$(AMIGAOS4_PACKAGE_DIR)/CDFileSystem-test"
	@cp "$(AMIGAOS4_README)" "$(AMIGAOS4_PACKAGE_DIR)/README.md"
	@echo "  LHA   $(AMIGAOS4_PACKAGE)"
	@(cd "$(AMIGAOS4_PACKAGE_DIR)" && \
		$(LHA) -aq "$(CURDIR)/$(AMIGAOS4_PACKAGE)" \
		ODFileSystem-amigaos4 CDFileSystem \
		ODFileSystem-amigaos4-test CDFileSystem-test README.md)
	@echo "  LHA archive ready: $(AMIGAOS4_PACKAGE)"

# ROM profile: minimal build for burning into ROM
# ISO9660 + Rock Ridge + Joliet + Multisession, no debug, no UDF/HFS/CDDA
#
# ROM builds use LTO — the single biggest size lever (about 6 KiB, 15%).
# It needs the gcc archiver wrapper: plain ar writes no LTO plugin symbol
# index, so startup.o's reference to handler_main goes unresolved at link
# time (this is what sank the earlier lto.diff attempt). Do NOT add
# -ffunction-sections/--gc-sections on top: measured 2 KiB WORSE than
# plain LTO and it garbage-collects the $VER: version string.
# -fno-strict-aliasing costs 84 bytes and insures against cross-TU
# aliasing assumptions becoming visible to the LTO optimizer.
# NOTE: stale non-LTO objects in an existing build/amiga-rom* directory
# fail the link with "undefined reference to handler_main" — remove the
# build directory when switching.
ROM_LTO ?= -flto -fno-strict-aliasing
rom:
	@$(MAKE) --no-print-directory \
		AMIGA_BUILD=$(ROM_BUILD) \
		AMIGA_OPT=-Os \
		LTO="$(ROM_LTO)" \
		AMIGA_AR=$(AMIGA_TOOL_PREFIX)-gcc-ar \
		CPPFLAGS="$(CPPFLAGS) -DODFS_PROFILE_ROM" \
		AMIGA_SIZE_LIMIT=$(ROM_SIZE_LIMIT) \
		SIZE_LIMIT_NAME=ROM_SIZE_LIMIT \
		SIZE_LIMIT_DESC=ROM\ profile\ handler \
		SERIAL_DEBUG=0 \
		FEATURE_UDF=0 \
		FEATURE_HFS=0 \
		FEATURE_HFSPLUS=0 \
		FEATURE_CDDA=0 \
		amiga
	@echo "  ROM profile build complete"

rom-test:
	@$(MAKE) --no-print-directory \
		AMIGA_BUILD=$(ROM_TEST_BUILD) \
		AMIGA_OPT=-Os \
		LTO="$(ROM_LTO)" \
		AMIGA_AR=$(AMIGA_TOOL_PREFIX)-gcc-ar \
		CPPFLAGS="$(CPPFLAGS) -DODFS_PROFILE_ROM" \
		ENFORCE_SIZE_LIMITS=0 \
		SERIAL_DEBUG=1 \
		FEATURE_UDF=0 \
		FEATURE_HFS=0 \
		FEATURE_HFSPLUS=0 \
		FEATURE_CDDA=0 \
		amiga
	@echo "  ROM test profile build complete"

# Print size breakdown of Amiga library objects
size: $(AMIGA_BUILD)/libodfs.a
	@echo "=== Amiga object sizes ==="
	@$(AMIGA_SIZE) $(AMIGA_BUILD)/libodfs.a

# ---- host library ----

lib: $(HOST_BUILD)/libodfs.a

$(HOST_BUILD)/libodfs.a: $(HOST_LIB_OBJS)
	@mkdir -p $(@D)
	@echo "  AR    $@"
	@$(AR) rcs $@ $^

# ---- host object files ----

$(HOST_BUILD)/%.o: %.c
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) $(HOSTCFLAGS) -c -o $@ $<

# ---- host tests ----

tests: $(TEST_BINS)

$(HOST_BUILD)/tests/test_%: tests/unit/test_%.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) -I tests/unit $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

check: tests
	@echo "=== Running unit tests ==="
	@failures=0; \
	for t in $(TEST_BINS); do \
	    echo "--- $$(basename $$t) ---"; \
	    $$t || failures=$$((failures + 1)); \
	    echo; \
	done; \
	if [ $$failures -ne 0 ]; then \
	    echo "$$failures test suite(s) failed"; \
	    exit 1; \
	fi; \
	echo "All test suites passed"

golden-check: tools
	@echo "=== Running golden image tests ==="
	@TOOLS="$(PWD)/$(HOST_BUILD)/tools" sh tests/golden/test_formats.sh
	@TOOLS="$(PWD)/$(HOST_BUILD)/tools" FETCH="$(PWD)/tests/golden/fetch_real_as_fixture.sh" sh tests/golden/test_as_real.sh

malformed-check: tools
	@echo "=== Running malformed image tests ==="
	@TOOLS="$(PWD)/$(HOST_BUILD)/tools" tests/malformed/test_malformed.sh

fuzz: $(FUZZ_BINS)

$(HOST_BUILD)/tests/fuzz_%: tests/fuzz/fuzz_%.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

fuzz-check: fuzz
	@echo "=== Running parser fuzz smoke tests ==="
	@FUZZ_BINS="$(PWD)/$(HOST_BUILD)/tests" tests/fuzz/run_fuzz.sh

integration-check: amiga-test
	@echo "=== Running AmiFUSE integration test ==="
	@ODFS_HANDLER="$(PWD)/$(TEST_HANDLER)" tests/integration/test_amifuse.sh

# ---- host tools ----

tools: $(TOOL_BINS)

$(HOST_BUILD)/tools/imginfo: tools/imginfo/imginfo.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

$(HOST_BUILD)/tools/imgls: tools/imgls/imgls.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

$(HOST_BUILD)/tools/imgcat: tools/imgcat/imgcat.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

$(HOST_BUILD)/tools/imgstat: tools/imgstat/imgstat.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

$(HOST_BUILD)/tools/imgbench: tools/imgbench/imgbench.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

$(HOST_BUILD)/tools/imgdump: tools/imgdump/imgdump.c $(HOST_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  HOSTCC $<"
	@$(HOSTCC) $(CPPFLAGS) $(INCLUDES) $(HOSTCFLAGS) -o $@ $< $(HOSTLDFLAGS) -L$(HOST_BUILD) -lodfs

# ---- Amiga library ----

$(AMIGA_BUILD)/libodfs.a: $(AMIGA_LIB_OBJS)
	@mkdir -p $(@D)
	@echo "  AR    $@ (amiga)"
	@$(AMIGA_AR) rcs $@ $^

# ---- Amiga object files ----

$(AMIGA_BUILD)/%.o: %.c
	@mkdir -p $(@D)
	@echo "  CC    $<"
	@$(CC) $(CPPFLAGS) $(AMIGA_INCLUDES) $(CFLAGS) -c -o $@ $<

# ---- Amiga assembly ----

$(AMIGA_BUILD)/%.o: %.S
	@mkdir -p $(@D)
	@echo "  AS    $<"
	@$(CC) $(CPPFLAGS) $(CFLAGS) -c -o $@ $<

# ---- Amiga test tools ----

$(AMIGA_TEST_BUILD)/tests/amiga/%.o: tests/amiga/%.c
	@mkdir -p $(@D)
	@echo "  CC    $<"
	@$(CC) $(CPPFLAGS) $(AMIGA_INCLUDES) $(CFLAGS) -c -o $@ $<

$(AMIGA_TEST_TOOL): $(AMIGA_TEST_BUILD)/tests/amiga/test_handler.o
	@mkdir -p $(@D)
	@echo "  LINK  $@"
	@$(CC) $(LDFLAGS) -o $@ $< $(LIBS)
	@echo "  STRIP $@"
	@$(STRIP) $@

# ---- Amiga handler ----

$(HANDLER): $(AMIGA_ASM_OBJS) $(AMIGA_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  LINK  $@"
	@$(CC) $(LDFLAGS) $(HANDLER_LDFLAGS) -o $@ $(AMIGA_ASM_OBJS) -L$(AMIGA_BUILD) -lodfs $(HANDLER_LIBS)
	@echo "  STRIP $@"
	@$(STRIP) $@

ifeq ($(AMIGA_TARGET),os4)
$(KICKSTART_MODULE): $(AMIGA_BUILD)/platform/amiga/os4/start.o $(AMIGA_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  LINK  $@ (kickstart)"
	@$(CC) $(LDFLAGS) -nostartfiles -nostdlib -Wl,-r -o $@.unstripped $< -L$(AMIGA_BUILD) -lodfs -lgcc
	@echo "  STRIP $@"
	@$(AMIGA_OBJCOPY) --strip-unneeded $@.unstripped $@
	@rm -f $@.unstripped
endif


# ==================================================================
# AMIX (SVR4/m68k) userspace cross build
# ==================================================================
#
# "ISO on a slice, no kernel relink" milestone: cross-builds libodfs.a
# plus the imgls/imgcat/imginfo tools for AMIX, pairing the shared core
# with the raw-device media backend (platform/amix/amix_file_media.c) —
# the exact parallel of the host build, which pairs the same core with
# platform/host/file_media.c. Artifacts land under build/amix/ so they
# never collide with the host or Amiga builds; this target is fully
# independent of default/host/check.
#
# The AMIX sysroot ships no <stdint.h>/<inttypes.h>, so platform/amix/
# compat is placed ahead of the (stdint-less) sysroot on the include
# path: the shared code's <stdint.h>/<inttypes.h> resolve to the shims
# there, which forward to include/odfs/compat_stdint.h. The cross gcc is
# a bash wrapper that already bakes in --sysroot and the crt/libc link,
# so no -sysroot flag is needed here.

# Root of the gcc-cross-amix toolchain (m68k-cbm-sysv4,
# https://github.com/jusii/gcc-cross-amix). Override it if the
# toolchain lives elsewhere:  make amix AMIX_CROSS_PREFIX=/path/to/amix-cross
AMIX_CROSS_PREFIX ?= /opt/amix-cross
AMIX_CC     ?= $(AMIX_CROSS_PREFIX)/bin/m68k-cbm-sysv4-gcc
# The cross binutils archiver (GNU ar 2.8.1) shipped with the toolchain
# aborts with a glibc _FORTIFY_SOURCE "buffer overflow detected" on any
# archive creation, so it cannot build libodfs.a. Bundling relocatable ELF
# objects is architecture-neutral and the cross ld reads the host ar's GNU
# armap, so the host ar is used instead. Override once the toolchain ships a
# working ar: make amix AMIX_AR=$(AMIX_CROSS_PREFIX)/bin/m68k-cbm-sysv4-ar
AMIX_AR     ?= ar

AMIX_BUILD  = build/amix

# compat/ must precede the (stdint-less) sysroot on the search path.
AMIX_INCLUDES = -I platform/amix/compat -I include -I backends
AMIX_DEFS     = -DODFS_PLATFORM_AMIX
AMIX_CFLAGS   = -O2 $(AMIX_DEFS)

# The SCSI-passthrough backend (amix_scsi_media.c) needs the kernel's
# `struct sdcom` + SDCARDS/SDUNITS definitions, which live in the cross
# sysroot under usr/sys/amiga/alien (rico.h/sd.h) and are NOT on the
# default include path. Scope that -I to just that one TU (see the
# per-object override below) so the rest of the AMIX build — and the
# portable headers — never see the kernel's aggressive short-name macros
# (bool/uint/and/or/not from rico.h).
AMIX_SYSROOT       ?= $(AMIX_CROSS_PREFIX)/m68k-cbm-sysv4/sysroot
AMIX_KERN_INCLUDES  = -I $(AMIX_SYSROOT)/usr/sys/amiga/alien

AMIX_LIB_SRCS = $(CORE_SRCS) platform/amix/amix_file_media.c \
                platform/amix/amix_scsi_media.c \
                platform/amiga/printf_local.c
AMIX_LIB_OBJS = $(patsubst %.c,$(AMIX_BUILD)/%.o,$(AMIX_LIB_SRCS))

AMIX_TOOL_NAMES = imginfo imgls imgcat
AMIX_TOOL_BINS  = $(patsubst %,$(AMIX_BUILD)/tools/%,$(AMIX_TOOL_NAMES))

# The SVR4 userland mount helper, installed on the box as /usr/lib/fs/cdfs/mount
# (mode 755). SVR4's generic mount(1M) mounts nothing itself — it execs
# /usr/lib/fs/<fstype>/mount — so without this binary `mount -F cdfs` fails
# outright and the whole CD stack is unreachable.
#
# It is built with NEITHER $(AMIX_INCLUDES) NOR $(AMIX_DEFS): the helper is a
# bare mount(2) + /etc/mnttab wrapper over system headers alone, depending on
# nothing in this repo. That is load-bearing rather than incidental — it keeps
# the on-box recovery build working, where the box's own 1991 K&R cc cannot see
# this tree at all:
#
#     cc -O -o /usr/lib/fs/cdfs/mount mount.c
#
# Withholding the -I/-D here is what enforces that mechanically: the moment
# mount.c grows a dependency on an odfs header, this rule stops compiling.
AMIX_MOUNT_BIN    = $(AMIX_BUILD)/mount
AMIX_MOUNT_CFLAGS = -O2

amix: amix-lib amix-tools amix-mount
	@echo "  AMIX build complete: $(AMIX_BUILD)/libodfs.a + $(AMIX_TOOL_NAMES) + mount"

amix-lib: $(AMIX_BUILD)/libodfs.a

amix-tools: $(AMIX_TOOL_BINS)

amix-mount: $(AMIX_MOUNT_BIN)

$(AMIX_BUILD)/libodfs.a: $(AMIX_LIB_OBJS)
	@mkdir -p $(@D)
	@echo "  AR    $@ (amix)"
	@$(AMIX_AR) rcs $@ $^

$(AMIX_BUILD)/%.o: %.c
	@mkdir -p $(@D)
	@echo "  AMIXCC $<"
	@$(AMIX_CC) $(CPPFLAGS) $(AMIX_INCLUDES) $(AMIX_CFLAGS) -c -o $@ $<

# Only the SCSI backend pulls in the kernel headers (rico.h/sd.h); scope
# their include path to that TU so rico.h's short-name macros stay local.
$(AMIX_BUILD)/platform/amix/amix_scsi_media.o: AMIX_INCLUDES += $(AMIX_KERN_INCLUDES)

# One-shot compile+link per tool (mirrors the host tool rules). Passing
# -lodfs suppresses the wrapper's implicit libc, so -lc is explicit and
# ordered after -lodfs (which depends on it). -lgcc supplies the 64-bit
# soft-arithmetic helpers (__udivdi3/__umoddi3/__lshrdi3) the m68k has no
# instructions for, which the HFS/HFS+/cache paths pull in.
$(AMIX_BUILD)/tools/imginfo: tools/imginfo/imginfo.c $(AMIX_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  AMIXCC $<"
	@$(AMIX_CC) $(CPPFLAGS) $(AMIX_INCLUDES) $(AMIX_CFLAGS) -o $@ $< -L$(AMIX_BUILD) -lodfs -lc -lgcc

$(AMIX_BUILD)/tools/imgls: tools/imgls/imgls.c $(AMIX_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  AMIXCC $<"
	@$(AMIX_CC) $(CPPFLAGS) $(AMIX_INCLUDES) $(AMIX_CFLAGS) -o $@ $< -L$(AMIX_BUILD) -lodfs -lc -lgcc

$(AMIX_BUILD)/tools/imgcat: tools/imgcat/imgcat.c $(AMIX_BUILD)/libodfs.a
	@mkdir -p $(@D)
	@echo "  AMIXCC $<"
	@$(AMIX_CC) $(CPPFLAGS) $(AMIX_INCLUDES) $(AMIX_CFLAGS) -o $@ $< -L$(AMIX_BUILD) -lodfs -lc -lgcc

# Standalone: no -I, no -D, no libodfs (see the AMIX_MOUNT_BIN comment above).
# With no -l the cross wrapper links libc by itself, which is all this needs.
$(AMIX_MOUNT_BIN): platform/amix/mount.c
	@mkdir -p $(@D)
	@echo "  AMIXCC $<"
	@$(AMIX_CC) $(CPPFLAGS) $(AMIX_MOUNT_CFLAGS) -o $@ $<

# ==================================================================
# AMIX (SVR4/m68k) in-kernel build — provider foundation
# ==================================================================
#
# Proves the portable ODFS core + backends compile and link as an SVR4
# kernel object, paired with the in-kernel SCSI media backend
# (platform/amix-kernel/amix_kern_media.c). It reuses the userspace amix
# target's cross CC and baked-sysroot wrapper, but differs in three ways:
#
#   1. -DODFS_PLATFORM_AMIX_KERNEL selects the kernel arm at each of the
#      four portable seams (alloc via kmem, no <stdio.h>, the bounded
#      formatter, the fixed-width-type shim) — no libc, no syscalls.
#   2. -D_KERNEL turns on the sys/*.h kernel declarations (systm.h gates
#      sleep/wakeup/bcopy on it; the alien sd.h ABI needs it too).
#   3. The objects are combined with the cross `ld -r` into ONE relocatable
#      object, NOT linked as an executable. The gcc wrapper always injects
#      crt1/crti/crtn + libc, so it is bypassed for the final link; ld -r
#      leaves every kernel/toolchain extern (kmem_*, sdqueue, sleep/wakeup,
#      spl*, vtop, __udivdi3/__umoddi3/__lshrdi3, memcpy/memset/...)
#      undefined, to be resolved at the real kernel link. No -lgcc: the
#      three 64-bit soft-arithmetic helpers are LEFT undefined on purpose.

AMIX_LD          ?= $(AMIX_CROSS_PREFIX)/bin/m68k-cbm-sysv4-ld
AMIX_KERNEL_BUILD = build/amix-kernel

# Two compat/ dirs precede the (stdint-less, memcpy-less) sysroot on the search
# path: platform/amix-kernel/compat supplies the <string.h> interceptor that
# routes memcpy/memset/memcmp/strlen to the cdfs-provided kstring.c bodies (the
# SVR4 kernel exports only bcopy/bzero), and platform/amix/compat supplies the
# <stdint.h>/<inttypes.h> shims forwarding to include/odfs/compat_stdint.h. The
# amix-kernel one must come FIRST so its string.h wins over the sysroot's.
AMIX_KERNEL_INCLUDES = -I platform/amix-kernel/compat -I platform/amix/compat \
                       -I include -I backends
AMIX_KERNEL_DEFS     = -DODFS_PLATFORM_AMIX_KERNEL -D_KERNEL
AMIX_KERNEL_CFLAGS   = -O2 $(AMIX_KERNEL_DEFS)

# The portable core/backends plus the in-kernel SCSI media backend and the
# vfsops/vnodeops/node-cache glue (cdfs_*.c) that makes the core mountable, plus
# kstring.c (the freestanding memcpy/memset/memcmp/strlen the SVR4 kernel lacks)
# and the bounded formatter.
AMIX_KERNEL_LIB_SRCS = $(CORE_SRCS) \
                       platform/amix-kernel/amix_kern_media.c \
                       platform/amix-kernel/cdfs_vfsops.c \
                       platform/amix-kernel/cdfs_vnodeops.c \
                       platform/amix-kernel/cdfs_node.c \
                       platform/amix-kernel/kstring.c \
                       platform/amiga/printf_local.c
AMIX_KERNEL_LIB_OBJS = $(patsubst %.c,$(AMIX_KERNEL_BUILD)/%.o,$(AMIX_KERNEL_LIB_SRCS))

AMIX_KERNEL_OBJ = $(AMIX_KERNEL_BUILD)/libcdfskern.o

amix-kernel: $(AMIX_KERNEL_OBJ)
	@echo "  AMIX-KERNEL build complete: $(AMIX_KERNEL_OBJ)"

$(AMIX_KERNEL_OBJ): $(AMIX_KERNEL_LIB_OBJS)
	@mkdir -p $(@D)
	@echo "  LD -r  $@"
	@$(AMIX_LD) -r -o $@ $(AMIX_KERNEL_LIB_OBJS)

$(AMIX_KERNEL_BUILD)/%.o: %.c
	@mkdir -p $(@D)
	@echo "  KAMIXCC $<"
	@$(AMIX_CC) $(CPPFLAGS) $(AMIX_KERNEL_INCLUDES) $(AMIX_KERNEL_CFLAGS) -c -o $@ $<

# Only the kernel media backend pulls the alien SCSI headers (rico.h/sd.h);
# scope their -I to that one TU so rico.h's short-name macros stay local —
# same pattern the userspace amix target uses for amix_scsi_media.o.
$(AMIX_KERNEL_BUILD)/platform/amix-kernel/amix_kern_media.o: \
	AMIX_KERNEL_INCLUDES += $(AMIX_KERN_INCLUDES)

# kstring.c defines the freestanding memcpy/memmove/memset/memcmp/strlen. It
# MUST compile with -fno-builtin: otherwise GCC recognizes the byte loops as the
# very idioms it is defining and rewrites them into self-calls (infinite
# recursion) — the same freestanding trap platform/amiga/os4/freestanding.o
# guards against.
$(AMIX_KERNEL_BUILD)/platform/amix-kernel/kstring.o: \
	AMIX_KERNEL_CFLAGS += -fno-builtin

# ---- the loadable-module form's object ----
#
# The same sources plus platform/amix-kernel/cdfs_modlocal.c, with
# -DCDFS_MODULE, compiled with the same flags but against an amix-dlm ABI
# kit's headers (one kit per kernel line) instead of the toolchain sysroot:
#
#   make amix-kernel-module AMIX_KIT=/path/to/kit/k30
#
# gives build/amix-module/<line>/libcdfskern.o, which
# platform/amix-kernel/module/Makefile links into the module (the kit's
# EXTRA). Normally that Makefile runs this, with the kit's pinned compiler.
# The static object above does not change.
ifneq ($(AMIX_KIT),)
AMIX_KMOD_LINE     := $(shell sed -n 's/^LINE=//p' $(AMIX_KIT)/kit.conf)
AMIX_KMOD_BUILD     = build/amix-module/$(AMIX_KMOD_LINE)
AMIX_KMOD_LIB_SRCS  = $(AMIX_KERNEL_LIB_SRCS) platform/amix-kernel/cdfs_modlocal.c
AMIX_KMOD_LIB_OBJS  = $(patsubst %.c,$(AMIX_KMOD_BUILD)/kobj/%.o,$(AMIX_KMOD_LIB_SRCS))
AMIX_KMOD_OBJ       = $(AMIX_KMOD_BUILD)/libcdfskern.o
AMIX_KMOD_CFLAGS    = $(AMIX_KERNEL_CFLAGS) -DCDFS_MODULE
AMIX_KMOD_INCLUDES  = $(AMIX_KERNEL_INCLUDES)

amix-kernel-module: $(AMIX_KMOD_OBJ)
	@echo "  AMIX-KERNEL module object ($(AMIX_KMOD_LINE)): $(AMIX_KMOD_OBJ)"

$(AMIX_KMOD_OBJ): $(AMIX_KMOD_LIB_OBJS)
	@mkdir -p $(@D)
	@echo "  LD -r  $@"
	@$(AMIX_LD) -r -o $@ $(AMIX_KMOD_LIB_OBJS)

# The cross gcc wrapper takes its header root from AMIX_SYSROOT: the kit's.
$(AMIX_KMOD_BUILD)/kobj/%.o: %.c $(AMIX_KIT)/kit.conf
	@mkdir -p $(@D)
	@echo "  KMODCC $<"
	@AMIX_SYSROOT=$(AMIX_KIT)/sysroot $(AMIX_CC) $(CPPFLAGS) $(AMIX_KMOD_INCLUDES) \
		$(AMIX_KMOD_CFLAGS) -c -o $@ $<

$(AMIX_KMOD_BUILD)/kobj/platform/amix-kernel/amix_kern_media.o: \
	AMIX_KMOD_INCLUDES += -I $(AMIX_KIT)/stock/usr/sys/amiga/alien
$(AMIX_KMOD_BUILD)/kobj/platform/amix-kernel/kstring.o: \
	AMIX_KMOD_CFLAGS += -fno-builtin
else
amix-kernel-module:
	@echo "amix-kernel-module: set AMIX_KIT to a built amix-dlm ABI kit" >&2; exit 1
endif

# ---- clean ----

clean:
	@echo "  CLEAN"
	@rm -rf build

-include $(HOST_LIB_DEPS) $(AMIGA_LIB_DEPS) $(AMIGA_ASM_DEPS) \
	$(TEST_DEPS) $(FUZZ_DEPS) $(TOOL_DEPS)
