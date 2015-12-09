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

GIT_VERSION := $(shell git describe --abbrev=4 --dirty --always --tags)
PLATFORM = $(shell uname -i)

CFLAGS = -W -Wall -Werror -Wwrite-strings -Wextra -Os -g \
	-DGIT_VERSION=\"$(GIT_VERSION)\" \
	-I. -I/opt/genwqe/include -I../include -I../include/linux/uapi \
	-Wmissing-prototypes # -Wstrict-prototypes -Warray-bounds

LDFLAGS += -L/opt/genwqe/lib

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
# Enabling BUILD_SIMCODE=1 enables simulation version which builds and
# links against the pslse version of libcxl.
#

ifeq ($(PLATFORM),ppc64le)              # Enable libcxl by default
CONFIG_LIBCXL_PATH ?= ../../libcxl
endif

BUILD_SIMCODE ?= 0

ifeq ($(BUILD_SIMCODE),1)               # Use simulation version of libcxl
CONFIG_LIBCXL_PATH = ../../pslse/libcxl
CFLAGS += -DCONFIG_BUILD_SIMCODE
endif

ifneq ($(CONFIG_LIBCXL_PATH),)          # Use libcxl
CFLAGS += -I$(CONFIG_LIBCXL_PATH) -I$(CONFIG_LIBCXL_PATH)/include
LDFLAGS += -L$(CONFIG_LIBCXL_PATH)
libcxl_a = $(CONFIG_LIBCXL_PATH)/libcxl.a
endif

# z_ prefixed version of libz, intended to be linked statically with
# our libz version to provide the software zlib functionality.
#
CONFIG_DLOPEN_MECHANISM ?= 1

ifeq ($(CONFIG_DLOPEN_MECHANISM),1)
CFLAGS += -DCONFIG_DLOPEN_MECHANISM
else
CONFIG_LIBZ_PATH=../zlib-1.2.8
libz_a=libz_prefixed.o
endif
