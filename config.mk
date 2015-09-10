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
CXX		= $(CROSS)g++
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
CXX		= printf "\t[CXX]\t%s\n" `basename "$@"`; $(CROSS)g++
AS		= printf "\t[AS]\t%s\n" `basename "$@"`; $(CROSS)as
LD		= printf "\t[LD]\t%s\n" `basename "$@"`; $(CROSS)ld
else
CLEAN		= echo -n
endif

GIT_VERSION := $(shell git describe --abbrev=4 --dirty --always --tags)
PLATFORM = $(shell uname -i)

CFLAGS = -W -Wall -Werror -Wwrite-strings -Wextra -Os -g \
	-DGIT_VERSION=\"$(GIT_VERSION)\" \
	-I../include -I../include/linux/uapi \
	-Wmissing-prototypes # -Wstrict-prototypes -Warray-bounds

CXXFLAGS = -Os -g -Wall -Werror -pipe -fno-rtti -fno-exceptions \
	-DGIT_VERSION=\"$(GIT_VERSION)\"

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
CXXFLAGS += -m32
LDFLAGS += -m32
XLDFLAGS = -melf_i386
ARFLAGS =
else
CFLAGS += -m64
CXXFLAGS += -m64
LDFLAGS += -m64
XLDFLAGS = -melf_x86_64
ARFLAGS =
endif
else
ARFLAGS =
endif
