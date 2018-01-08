#
# Copyright 2015, International Business Machines
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

# Verbosity level:
#   V=0 means completely silent
#   V=1 means brief output
#   V=2 means full output
#
V		?= 1
CC		= $(CROSS)gcc
AS		= $(CROSS)as
LD		= $(CROSS)ld
AR		= $(CROSS)ar
RANLIB		= $(CROSS)ranlib
OBJCOPY		= $(CROSS)objcopy
OBJDUMP		= $(CROSS)objdump
STRIP		= $(CROSS)strip
NM		= $(CROSS)nm
HELP2MAN	= help2man

ifeq ($(V),0)
Q		:= @
MAKEFLAGS	+= --silent
MAKE		+= -s
endif

ifeq ($(V),1)
MAKEFLAGS	+= --silent
MAKE		+= -s
CC		= printf "\t[CC]\t%s\n" `basename "$@"`; $(CROSS)gcc
AS		= printf "\t[AS]\t%s\n" `basename "$@"`; $(CROSS)as
AR		= printf "\t[AR]\t%s\n" `basename "$@"`; $(CROSS)ar
LD		= printf "\t[LD]\t%s\n" `basename "$@"`; $(CROSS)ld
OBJCOPY		= printf "\t[OBJCOPY]\t%s\n" `basename "$@"`; $(CROSS)objcopy
else
CLEAN		= echo -n
endif

#
# If we can use git to get a version, we use that. If not, we have
# no repository and set a static version number.
#
# NOTE Keep the VERSION for the non git case in sync with the git
#      tag used to build this code!
#
HAS_GIT = $(shell git describe > /dev/null 2>&1 && echo y || echo n)

ifeq (${HAS_GIT},y)
VERSION ?= $(shell git describe --abbrev=4 --always --tags | sed -e 's/v//g')
RPMVERSION ?= $(shell git describe --abbrev=0 --tags | cut -c 2-7)
else
VERSION=4.0.20
RPMVERSION=$(VERSION)
endif
MAJOR_VERS=$(shell echo $(VERSION) | cut -d'.' -f1)

PLATFORM ?= $(shell uname -i)

CFLAGS ?= -W -Wall -Werror -Wwrite-strings -Wextra -O2 -g \
	-Wmissing-prototypes # -Wstrict-prototypes -Warray-bounds
CFLAGS += -DGIT_VERSION=\"$(VERSION)\" \
	-I. -I../include -I../include/linux/uapi -D_GNU_SOURCE=1

# Force 32-bit build
#   This is needed to generate the code for special environments. We have
#   some 64-bit machines where we need to support binaries compiled for
#   32-bit.
#
#   FORCE_32BIT=0  Use machine default
#   FORCE_32BIT=1  Enforce 32-bit build
#
ifeq ($(PLATFORM),x86_64)
FORCE_32BIT     ?= 0
ifeq ($(FORCE_32BIT),1)
CFLAGS += -m32
LDFLAGS += -m32
XLDFLAGS = -melf_i386
ARFLAGS =
else
CFLAGS += -m64
LDFLAGS += -m64
XLDFLAGS = -melf_x86_64
ARFLAGS =
endif
else
ARFLAGS =
endif

# Libcxl is required to run the CAPI version of this code. Libcxl is
# available for normal CAPI/PCIe device usage, but also as simulation
# version, which connects to the pslse server, which talks to the
# hardware simulator.
#
# libcxl is enabled by default on architectures that support
# libcxl (ppc64le).
#
# If you need to disable it, you can run Make with DISABLE_LIBCXL=1.
#
# If you want to use the bundled version of libcxl (*not recommended*),
# run make with BUNDLE_LIBCXL=1. If your bundle is in some place other
# than ../ext/libcxl, you can use CONFIG_LIBCXL_PATH to fix it.
#
# If you want to use the simulation (pslse) version of libcxl, run with
# BUILD_SIMCODE=1. If your bundle is in some place other than
# ../../pslse/libcxl, you can use CONFIG_LIBCXL_PATH to fix it.
#
#
# libcxl cannot be enabled on platforms that don't have CAPI support.

ifndef DISABLE_LIBCXL

ifeq ($(PLATFORM), ppc64le)
WITH_LIBCXL=1
endif

ifeq ($(PLATFORM), ppc64)
WITH_LIBCXL=1
endif

ifdef BUILD_SIMCODE
WITH_LIBCXL=1
BUNDLE_LIBCXL ?= 1
CONFIG_LIBCXL_PATH ?= ../../pslse/libcxl
CFLAGS += -DCONFIG_BUILD_SIMCODE -I../ext/include
endif

CFLAGS += -I../include
# Can be overwritten by makfile option
ifeq ($(BUNDLE_LIBCXL),1)
WITH_LIBCXL=1
CONFIG_LIBCXL_PATH ?= ../ext/libcxl
CFLAGS += -I../ext/include
endif

# Finally, set any path needed.
ifdef CONFIG_LIBCXL_PATH
CFLAGS += -I$(CONFIG_LIBCXL_PATH) -I$(CONFIG_LIBCXL_PATH)/include
LDFLAGS += -L$(CONFIG_LIBCXL_PATH)
libcxl_a = $(CONFIG_LIBCXL_PATH)/libcxl.a
endif # !CONFIG_LIBCXL_PATH

endif # !DISABLE_LIBCXL

# z_ prefixed version of libz, intended to be linked statically with
# our libz version to provide the software zlib functionality.
# Allow overwriting the ZLIB_PATH to the right libz.so file e.g. via
# spec file during RPM build.
#
CONFIG_DLOPEN_MECHANISM ?= 1

# CONFIG_ZLIB_PATH ?= /usr/lib64/libz.so.1
CONFIG_ZLIB_PATH ?= $(shell /sbin/ldconfig -p | grep libz.so.1 | cut -d' ' -f4 | head -n1)

ifeq ($(CONFIG_DLOPEN_MECHANISM),1)
CFLAGS += -DCONFIG_DLOPEN_MECHANISM -DCONFIG_ZLIB_PATH=\"$(CONFIG_ZLIB_PATH)\"
else
CONFIG_LIBZ_PATH=../zlib-1.2.8
CFLAGS += -I$(CONFIG_LIBZ_PATH)
libz_a=libz_prefixed.o
endif

DESTDIR ?= /usr
LIB_INSTALL_PATH ?= $(DESTDIR)/lib64/genwqe
INCLUDE_INSTALL_PATH ?= $(DESTDIR)/include/genwqe
MAN_INSTALL_PATH ?= $(DESTDIR)/share/man/man1
