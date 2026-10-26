# platform/amix-kernel/module/Makefile -- cdfs as an Amix loadable filesystem
# module, one per kernel line, built with amix-dlm's ABI kit (the kit's
# docs/guide.md; contracts C3-1 and C4-6). GNU make.
#
#   make -C platform/amix-kernel/module        both lines:
#                         build/amix-module/k30/mod.d/cdfs and
#                         build/amix-module/k46/mod.d/cdfs, each checked by
#                         the kit's modfix and modcheck.py, then by modgate.py
#   make -C platform/amix-kernel/module k30    one line (or k46)
#   make -C platform/amix-kernel/module test   the host test (hosttest.sh)
#   make -C platform/amix-kernel/module clean
#
# DLM is the amix-dlm checkout whose kits are built (`make kit` there gives
# build/kit/k30 and build/kit/k46); KIT_k30 / KIT_k46 name a kit directly.
# The kits must be kabi k30.1.2 / k46.1.2 or later (vfs_register, sdqueue and
# the other names cdfs imports). The pinned cross tools (m68k-cbm-sysv4-*)
# and python3 must be on PATH.
#
# Two compilers' worth of flags, one compiler: the portable sources are ANSI
# C and the kit compiles -traditional, so the repository's Makefile builds
# them (amix-kernel-module, -DCDFS_MODULE) with the kit's pinned compiler
# against the kit's headers into libcdfskern.o, passed to the kit as EXTRA.
# The kit compiles only the K&R wrapper, cdfs_mod.c. Master is generated
# from keys; it has no $cpu, so the kit gives each line its own.

TOP := $(abspath $(dir $(lastword $(MAKEFILE_LIST)))../../..)
DLM ?= $(abspath $(TOP)/../amix-dlm)
LINES = k30 k46
kit = $(or $(KIT_$(1)),$(DLM)/build/kit/$(1))

ifeq ($(KIT),)

all: $(LINES)

$(LINES):
	$(MAKE) KIT=$(call kit,$@)

test:
	sh $(TOP)/platform/amix-kernel/module/hosttest.sh

clean: $(LINES:%=clean-%)

$(LINES:%=clean-%): clean-%:
	$(MAKE) KIT=$(call kit,$*) clean

.PHONY: all test clean $(LINES) $(LINES:%=clean-%)

else	# one line, the kit's

MODULE = cdfs
OBJS   = cdfs_mod.o
MODOUT = $(TOP)/build/amix-module/$(LINE)
EXTRA  = $(MODOUT)/libcdfskern.o
vpath %.c $(TOP)/platform/amix-kernel
include $(KIT)/mk/module.mk

# The -DCDFS_MODULE object: the repository Makefile decides whether it is
# out of date. KIT is cleared for that make (it is this one's variable).
$(EXTRA): FORCE
	$(MAKE) -C $(TOP) KIT= amix-kernel-module AMIX_KIT=$(KIT) \
		AMIX_CC=$(KIT_CC) AMIX_LD=m68k-cbm-sysv4-ld

Master: keys
	{ echo '# generated from keys by module/Makefile'; \
	  echo '$$modtype misc'; \
	  sed -n 's/^\([a-z]*\)=\(.*\)$$/$$\1 \2/p' keys | grep -v '^\$$name '; \
	  echo '$(MODULE) cdfs'; } > Master

# After the kit's checks: run-in-place readiness, the DDI-only import list,
# the .moddata keys and the sha256.
all: gate

gate: $(MODOUT)/mod.d/$(MODULE)
	python3 modgate.py $(KIT) $<

FORCE:
.PHONY: gate FORCE

endif
